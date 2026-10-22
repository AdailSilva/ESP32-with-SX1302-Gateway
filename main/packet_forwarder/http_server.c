/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2023 Semtech

Description:
    Http service implementation for configure from browser

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>
#include "esp_timer.h"
#include <time.h>
#include "esp_console.h"
#include "argtable3/argtable3.h"

#include "http_server.h"
#include "webpage.h"
#include "web_config.h"
#include "global_json.h"
#include "branding.h"
#include "mqtt_gw.h"
#include "weblog.h"
#include "logs_page.h"
#include "diag.h"
#include "diag_page.h"
#include "loragw_aux.h"
#include "loragw_version.h"
#include "sdkconfig.h"


static const char *TAG = "esp32 web server";

typedef struct {
    char    *username;
    char    *password;
} basic_auth_info_t;
basic_auth_info_t web_auth_info;

char web_username[32] = BASIC_AUTH_USERNAME;
char web_password[32] = BASIC_AUTH_PASSWORD;

extern config_s config[CONFIG_NUM];


/* ---- web page login (saved in NVS as web_user / web_pswd) ---- */
static bool web_auth_valid(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > WEB_AUTH_MAX_LEN)
        return false;
    for (; *s; s++)
        if (*s == ':' || (unsigned char)*s < 0x20)   /* ':' is the user/password separator */
            return false;
    return true;
}

void web_auth_apply(void)
{
    const char *u = (config[WEB_USER].val && config[WEB_USER].len) ? config[WEB_USER].val : NULL;
    const char *p = (config[WEB_PSWD].val && config[WEB_PSWD].len) ? config[WEB_PSWD].val : NULL;

    if (u && strcmp(u, WEB_LOGOUT_USER) == 0)   /* reserved for the logout button */
        u = NULL;
    snprintf(web_username, sizeof(web_username), "%s", web_auth_valid(u) ? u : BASIC_AUTH_USERNAME);
    snprintf(web_password, sizeof(web_password), "%s", web_auth_valid(p) ? p : BASIC_AUTH_PASSWORD);
    web_auth_info.username = web_username;
    web_auth_info.password = web_password;
}

static struct {
    struct arg_str *user, *pass;
    struct arg_lit *reset;
    struct arg_end *end;
} web_auth_args;

static int cmd_web_auth(int argc, char **argv)
{
    char *v;

    if (arg_parse(argc, argv, (void **)&web_auth_args) != 0) {
        arg_print_errors(stderr, web_auth_args.end, argv[0]);
        return 1;
    }
    if (web_auth_args.reset->count) {
        free(config[WEB_USER].val); config[WEB_USER].val = strdup(BASIC_AUTH_USERNAME);
        config[WEB_USER].len = strlen(BASIC_AUTH_USERNAME);
        free(config[WEB_PSWD].val); config[WEB_PSWD].val = strdup(BASIC_AUTH_PASSWORD);
        config[WEB_PSWD].len = strlen(BASIC_AUTH_PASSWORD);
    }
    if (web_auth_args.user->count) {
        if (!web_auth_valid(web_auth_args.user->sval[0]) || strcmp(web_auth_args.user->sval[0], WEB_LOGOUT_USER) == 0) {
            printf("ERROR: user must have 1-%d characters, without ':' (and not \"" WEB_LOGOUT_USER "\")\n", WEB_AUTH_MAX_LEN);
            return 1;
        }
        v = strdup(web_auth_args.user->sval[0]);
        free(config[WEB_USER].val); config[WEB_USER].val = v; config[WEB_USER].len = strlen(v);
    }
    if (web_auth_args.pass->count) {
        const char *np = web_auth_args.pass->sval[0];
        if (!web_auth_valid(np) || strlen(np) < 6) {
            printf("ERROR: password must have 6-%d characters, without ':'\n", WEB_AUTH_MAX_LEN);
            return 1;
        }
        v = strdup(np);
        free(config[WEB_PSWD].val); config[WEB_PSWD].val = v; config[WEB_PSWD].len = strlen(v);
    }
    if (web_auth_args.reset->count || web_auth_args.user->count || web_auth_args.pass->count) {
        save_config();
        web_auth_apply();
        printf("Web login updated (applied now).\n");
    }
    printf("Web login user: %s   password: %s\n", web_username,
           strcmp(web_password, BASIC_AUTH_PASSWORD) == 0 ? "(default)" : "(custom)");
    return 0;
}

void web_auth_register_console(void)
{
    web_auth_args.user  = arg_str0(NULL, "user", "<user>", "new web user");
    web_auth_args.pass  = arg_str0(NULL, "pass", "<password>", "new web password (min. 6 characters)");
    web_auth_args.reset = arg_lit0(NULL, "reset", "back to the default login admin / lorawan101");
    web_auth_args.end   = arg_end(3);
    const esp_console_cmd_t cmd = {
        .command = "web_auth",
        .help = "Show or change the login of the web page (applied immediately)",
        .hint = NULL,
        .func = &cmd_web_auth,
        .argtable = &web_auth_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static char *http_auth_basic(const char *username, const char *password)
{
    int out;
    char *user_info = NULL;
    char *digest = NULL;
    size_t n = 0;

    asprintf(&user_info, "%s:%s", username, password);
    if (!user_info) {
        ESP_LOGE(TAG, "No enough memory for user information");
        return NULL;
    }
    esp_crypto_base64_encode(NULL, 0, &n, (const unsigned char *)user_info, strlen(user_info));

    /* 6: The length of the "Basic " string
     * n: Number of bytes for a base64 encode format
     * 1: Number of bytes for a reserved which be used to fill zero
    */
    digest = calloc(1, 6 + n + 1);
    if (digest) {
        strcpy(digest, "Basic ");
        esp_crypto_base64_encode((unsigned char *)digest + 6, n, (size_t *)&out, (const unsigned char *)user_info, strlen(user_info));
    }

    free(user_info);
    return digest;
}

static esp_err_t check_basic_auth(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = 0;
    basic_auth_info_t *basic_auth_info = &web_auth_info;

    buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len == 1) {
        ESP_LOGE(TAG, "No auth header received");
        return ESP_FAIL;
    }

    buf = calloc(1, buf_len);
    if (!buf) {
        ESP_LOGE(TAG, "No enough memory for basic authorization");
        return ESP_ERR_NO_MEM;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
        ESP_LOGI(TAG, "Found header => Authorization: %s", buf);
    } else {
        ESP_LOGE(TAG, "No auth value received");
        free(buf);
        return ESP_FAIL;
    }

    char *auth_credentials = http_auth_basic(basic_auth_info->username,
            basic_auth_info->password);
    if (!auth_credentials) {
        ESP_LOGE(TAG, "No enough memory for basic authorization credentials");
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    if (strncmp(auth_credentials, buf, buf_len)) {
        ESP_LOGE(TAG, "Authenticated failed");
        free(auth_credentials);
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Authenticated!");
    free(auth_credentials);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_basic_auth(httpd_req_t *req)
{
    esp_err_t err = check_basic_auth(req);
    if(err == ESP_FAIL){
        ESP_LOGE(TAG, "No auth header received");
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"\"");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * The page is sent in chunks while the saved values are injected into it,
 * so no big RAM buffer is needed. The injection points are searched in the
 * same order as they appear in webpage.html.
 * ------------------------------------------------------------------------- */
typedef struct {
    httpd_req_t *req;
    const char *last;     /* first byte of webpage_str not sent yet */
} page_ctx_t;

static void pg_emit_to(page_ctx_t *c, const char *upto)
{
    if (upto > c->last)
        httpd_resp_send_chunk(c->req, c->last, upto - c->last);
    c->last = upto;
}

static void pg_text(page_ctx_t *c, const char *text)
{
    httpd_resp_send_chunk(c->req, text, strlen(text));
}

/* insert text right before / after the next occurrence of needle */
static void pg_before(page_ctx_t *c, const char *needle, const char *text)
{
    const char *p = strstr(c->last, needle);
    if (p) { pg_emit_to(c, p); pg_text(c, text); }
}

static void pg_after(page_ctx_t *c, const char *needle, const char *text)
{
    const char *p = strstr(c->last, needle);
    if (p) { pg_emit_to(c, p + strlen(needle)); pg_text(c, text); }
}

static const char *cfg_value(int tag)
{
    return (config[tag].val != NULL && config[tag].len > 0) ? config[tag].val : NULL;
}

/* value='...' with HTML escaping, inserted before the field name */
static void pg_value(page_ctx_t *c, const char *name_attr, const char *val)
{
    char buf[220];
    size_t j;

    if (val == NULL)
        return;
    j = snprintf(buf, sizeof(buf), "value='");
    for (; *val && j < sizeof(buf) - 10; val++) {
        switch (*val) {
        case '\'': j += snprintf(buf + j, sizeof(buf) - j, "&#39;"); break;
        case '"':  j += snprintf(buf + j, sizeof(buf) - j, "&quot;"); break;
        case '&':  j += snprintf(buf + j, sizeof(buf) - j, "&amp;"); break;
        case '<':  j += snprintf(buf + j, sizeof(buf) - j, "&lt;"); break;
        default:   buf[j++] = *val; break;
        }
    }
    snprintf(buf + j, sizeof(buf) - j, "' ");
    pg_before(c, name_attr, buf);
}

static void send_webpage(httpd_req_t *req)
{
    page_ctx_t c = { .req = req, .last = webpage_str };
    const char *v;
    char needle[48];

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    v = cfg_value(WIFI_MODE);
    if (v && strcmp(v, "soft_ap") == 0)
        pg_before(&c, "value='soft_ap'", "checked ");
    else if (v && strcmp(v, "station") == 0)
        pg_before(&c, "value='station'", "checked ");

    v = cfg_value(FREQ_REGION);
    if (v && strlen(v) < 16) {
        snprintf(needle, sizeof(needle), "value='%s'", v);
        pg_after(&c, needle, " checked");
    }

    pg_value(&c, "name='freq_radio0'", cfg_value(FREQ_RADIO0));
    pg_value(&c, "name='freq_radio1'", cfg_value(FREQ_RADIO1));
    pg_value(&c, "name='wifi_ssid'", cfg_value(WIFI_SSID));
    if (cfg_value(WIFI_PASSWORD))
        pg_before(&c, "name='wifi_pswd'", "placeholder='Salva (vazio mantém)' ");
    v = cfg_value(IP_MODE);
    pg_after(&c, (v && strcmp(v, "static") == 0) ? "name='ip_mode' value='static'" : "name='ip_mode' value='dhcp'", " checked");
    pg_value(&c, "name='ip_addr'", cfg_value(IP_ADDR));
    pg_value(&c, "name='ip_mask'", cfg_value(IP_MASK));
    pg_value(&c, "name='ip_gw'", cfg_value(IP_GW));
    pg_value(&c, "name='ip_dns1'", cfg_value(IP_DNS1));
    pg_value(&c, "name='ip_dns2'", cfg_value(IP_DNS2));
    pg_value(&c, "name='ns_host'", cfg_value(NS_HOST));
    pg_value(&c, "name='ns_port'", cfg_value(NS_PORT));
    pg_value(&c, "name='gw_id'", cfg_value(GW_ID));
    pg_value(&c, "name='ntp_server'", cfg_value(NTP_SERVER) ? cfg_value(NTP_SERVER) : NTP_SERVER_ADDR);

    v = cfg_value(MQTT_EN);
    pg_after(&c, (v && strcmp(v, "on") == 0) ? "name='mqtt_en' value='on'" : "name='mqtt_en' value='off'", " checked");
    v = cfg_value(MQTT_FMT);
    pg_after(&c, (v && strcmp(v, "chirpstack") == 0) ? "name='mqtt_fmt' value='chirpstack'" : "name='mqtt_fmt' value='semtech'", " checked");
    v = cfg_value(MQTT_ENC);
    pg_after(&c, (v && strcmp(v, "protobuf") == 0) ? "name='mqtt_enc' value='protobuf'" : "name='mqtt_enc' value='json'", " checked");
    pg_value(&c, "name='mqtt_uri'", cfg_value(MQTT_URI));
    pg_value(&c, "name='mqtt_user'", cfg_value(MQTT_USER));
    if (cfg_value(MQTT_PSWD))
        pg_before(&c, "name='mqtt_pswd'", "placeholder='Salva (vazio mantém)' ");
    pg_value(&c, "name='mqtt_topic'", cfg_value(MQTT_TOPIC));
    pg_value(&c, "name='web_user'", web_username);

    pg_emit_to(&c, c.last + strlen(c.last));   /* rest of the page */
    httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t gw_config_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    esp_err_t err;

    err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    send_webpage(req);
    return ESP_OK;
}

/* small page used after "Salvar" and "Reiniciar" (same look as webpage.html) */
#define SIMPLE_PAGE_HEAD \
    "<!DOCTYPE html><html lang='pt-BR'><head><meta charset='utf-8'>" \
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>" GW_NAME_SHORT "</title>" \
    "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>" \
    "<style>body{margin:0;min-height:100vh;display:grid;place-items:center;padding:16px;box-sizing:border-box;" \
    "background:#eef2f0;color:#14211d;font:16px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}" \
    "@media(prefers-color-scheme:dark){body{background:#0d1412;color:#e2ebe7}}" \
    "div{max-width:440px;text-align:center}h2{margin:0 0 8px}p{opacity:.8}" \
    "button{font:inherit;padding:10px 16px;margin:6px;border-radius:8px;cursor:pointer;" \
    "border:1px solid #0b7a5f;background:transparent;color:inherit}.p{background:#0b7a5f;color:#fff;font-weight:600}" \
    "form{display:inline}.dev{margin-top:18px;font-size:12.5px;opacity:.65}.dev a{color:inherit}</style>"

/* end of those pages, with the developer credit */
#define SIMPLE_PAGE_END "<p class='dev'>" GW_NAME " · Desenvolvido por <a href='https://k8s.adailsilva.com.br/' target='_blank' rel='noopener'><b>AdailSilva</b></a></p></div></body></html>"

static esp_err_t gw_response_handler(httpd_req_t *req)
{
    static char buf[2048];  /* all form fields filled can exceed 256 bytes */
    size_t total = 0, want;

    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;

    want = MIN(req->content_len, sizeof(buf) - 1);
    if (req->content_len > want)
        ESP_LOGW(TAG, "Form data too long (%u bytes), truncated to %u", (unsigned)req->content_len, (unsigned)want);
    while (total < want) {
        int ret = httpd_req_recv(req, buf + total, want - total);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        total += ret;
    }
    buf[total] = '\0';
    {   /* log the form without the passwords (the log is visible on the /logs page) */
        static char shown[sizeof(buf) + 16];
        const char *secret[] = { "wifi_pswd=", "mqtt_pswd=", "web_pswd=" };
        snprintf(shown, sizeof(shown), "%s", buf);
        for (int i = 0; i < 3; i++) {
            char *p = strstr(shown, secret[i]);
            if (p && (p == shown || p[-1] == '&')) {
                p += strlen(secret[i]);
                if (*p && *p != '&') {
                    char *e = strchr(p, '&');
                    size_t rest = e ? strlen(e) : 0;
                    memmove(p + 3, e ? e : p + strlen(p), rest + 1);
                    memcpy(p, "***", 3);
                }
            }
        }
        ESP_LOGI(TAG, "Form data: %s", shown);
    }

    char old_user[32], old_pass[32];
    snprintf(old_user, sizeof(old_user), "%s", web_username);
    snprintf(old_pass, sizeof(old_pass), "%s", web_password);

    extract_data_items(buf);
    save_config();
    mqtt_gw_start();   /* MQTT settings take effect immediately */
    web_auth_apply();  /* new login takes effect immediately */
    bool login_changed = strcmp(old_user, web_username) != 0 || strcmp(old_pass, web_password) != 0;

    const char *resp_str = NULL;
    if (login_changed) {
        resp_str = (const char *) SIMPLE_PAGE_HEAD "</head><body><div>"
                            "<h2>Configuração salva</h2>"
                            "<p>O usuário/senha da página foram alterados. O navegador vai pedir o novo login "
                            "ao continuar. As configurações de MQTT já foram aplicadas; as demais valem após reiniciar.</p>"
                            "<form action='/reboot' method='post'><button class='p' type='submit'>Reiniciar agora</button></form>"
                            "<form action='/' method='get'><button type='submit'>Voltar</button></form>"
                            SIMPLE_PAGE_END;
        httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    resp_str = (const char *) SIMPLE_PAGE_HEAD "</head><body><div>"
                            "<h2>Configuração salva</h2>"
                            "<p>As configurações de MQTT já foram aplicadas. As demais passam a valer depois que o gateway reiniciar.</p>"
                            "<form action='/reboot' method='post'><button class='p' type='submit'>Reiniciar agora</button></form>"
                            "<form action='/' method='get'><button type='submit'>Voltar</button></form>"
                            SIMPLE_PAGE_END;

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t gw_reboot_handler(httpd_req_t *req)
{
    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;

    ESP_LOGW(TAG, "Reboot required");

    const char *resp_str = NULL;
    resp_str = (const char *) SIMPLE_PAGE_HEAD "<meta http-equiv='refresh' content='15; URL=/'></head><body><div>"
                            "<h2>Reiniciando…</h2>"
                            "<p>A página volta sozinha em 15 segundos. Se o gateway mudou de modo "
                            "(Soft AP ↔ Station), acesse pelo novo endereço.</p>"
                            SIMPLE_PAGE_END;

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    wait_ms(500);
    esp_restart();

    return ESP_OK;
}

char json_conf_buf[1024*7];  // must hold the biggest JSON config (~5.9K) + "<pre></pre>"
static void assemble_json_str(char *str)
{
    int len = strlen(str);

    strcpy(json_conf_buf, "<pre>");
    strcpy(json_conf_buf+5, str);
    strcpy(json_conf_buf+5+len, "</pre>");
    json_conf_buf[5+len+6] = '\0';
}

static esp_err_t gw_json_conf_handler(httpd_req_t *req)
{
    esp_err_t err = handle_basic_auth(req);
    if(err == ESP_FAIL)
        return err;

    if(strcmp((const char *) req->user_ctx, "au915_sb0") == 0)
        assemble_json_str((char *)global_au0_conf + 2);
    else if(strcmp((const char *) req->user_ctx, "au915_sb1") == 0)
        assemble_json_str((char *)global_au1_conf + 2);
    else if(strncmp((const char *) req->user_ctx, "cn470", 5) == 0)
        assemble_json_str((char *)global_cn_conf + 2);  // ignore the first 2 bytes which is the length
    else if(strncmp((const char *) req->user_ctx, "eu868", 5) == 0)
        assemble_json_str((char *)global_eu_conf + 2);
    else if(strncmp((const char *) req->user_ctx, "us915", 5) == 0)
        assemble_json_str((char *)global_us_conf + 2);

    httpd_resp_send(req, (const char *)json_conf_buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /status -> {"gw":{...},"mqtt":{...}}
 * One request feeds the whole page (status panel + MQTT). The two parts are
 * sent as chunks reusing the same buffer, so the stack use is the same as
 * the old /mqtt_status (640 bytes). */
static esp_err_t status_handler(httpd_req_t *req)
{
    char buf[640];

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "{\"gw\":", HTTPD_RESP_USE_STRLEN);
    pkt_fwd_status_json(buf, sizeof(buf));
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, ",\"mqtt\":", HTTPD_RESP_USE_STRLEN);
    mqtt_gw_status_json(buf, sizeof(buf));
    httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET /config.json -> saved configuration, used by "Exportar configuração".
 * Passwords (Wi-Fi, MQTT, page) and the TLS certificates never leave the
 * gateway. Values are sent as they are stored in NVS ("" = not set, which the
 * form treats as "keep the current value"). Sent in chunks: no big buffer. */
static void cj_str(httpd_req_t *req, const char *s)
{
    char out[96];
    size_t j = 0;

    out[j++] = '"';
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (j > sizeof(out) - 8) { httpd_resp_send_chunk(req, out, j); j = 0; }
        if (ch == '"' || ch == '\\') { out[j++] = '\\'; out[j++] = ch; }
        else if (ch < 0x20) j += snprintf(out + j, sizeof(out) - j, "\\u%04x", ch);
        else out[j++] = ch;
    }
    out[j++] = '"';
    httpd_resp_send_chunk(req, out, j);
}

static void cj_kv(httpd_req_t *req, const char *key, const char *val)
{
    httpd_resp_send_chunk(req, ",\n  ", HTTPD_RESP_USE_STRLEN);
    cj_str(req, key);
    httpd_resp_send_chunk(req, ": ", HTTPD_RESP_USE_STRLEN);
    cj_str(req, val ? val : "");
}

static esp_err_t config_json_handler(httpd_req_t *req)
{
    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, "{\n  \"_type\": \"esp32_sx1302\"", HTTPD_RESP_USE_STRLEN);
    cj_kv(req, "_version", ESP32_SX1302_VERSION);
    cj_kv(req, "_target", CONFIG_IDF_TARGET);
    for (int i = 0; i < CONFIG_NUM; i++) {
        if (i == WIFI_PASSWORD || i == MQTT_PSWD || i == WEB_PSWD)
            continue;                                   /* secrets stay here */
        cj_kv(req, config[i].name, i == WEB_USER ? web_username : cfg_value(i));
    }
    httpd_resp_send_chunk(req, "\n}\n", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /mqtt_cert?type=ca|crt|key, body = PEM text (empty body removes it) */
static esp_err_t mqtt_cert_handler(httpd_req_t *req)
{
    char query[32], type[8] = "";
    char *body;
    size_t total = 0;
    esp_err_t err;

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        httpd_query_key_value(query, "type", type, sizeof(type));
    if (req->content_len > MQTT_GW_CERT_MAX) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"certificado grande demais\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    body = malloc(req->content_len + 1);
    if (!body)
        return ESP_FAIL;
    while (total < req->content_len) {
        int ret = httpd_req_recv(req, body + total, req->content_len - total);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            free(body);
            return ESP_FAIL;
        }
        total += ret;
    }
    body[total] = '\0';
    err = mqtt_gw_cert_set(type, body, total);
    free(body);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        mqtt_gw_start();
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err == ESP_ERR_INVALID_ARG
                        ? "{\"ok\":false,\"error\":\"não parece um certificado/chave PEM\"}"
                        : "{\"ok\":false,\"error\":\"falha ao gravar\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Logout. HTTP Basic has no real "logout": the browser keeps sending the
 * saved login. The page calls GET /logout through XMLHttpRequest with a fake
 * login (WEB_LOGOUT_USER); this handler accepts only that fake login, so the
 * browser replaces the saved one by it. Then the page goes to /saiu (no
 * authentication) and the next access to "/" asks for user and password again.
 * ------------------------------------------------------------------------- */
static esp_err_t logout_handler(httpd_req_t *req)
{
    char buf[96] = {0};
    char *fake = http_auth_basic(WEB_LOGOUT_USER, WEB_LOGOUT_PASS);
    bool ok = fake != NULL
              && httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) == ESP_OK
              && strcmp(buf, fake) == 0;
    free(fake);

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!ok) {
        httpd_resp_set_status(req, HTTPD_401);
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"\"");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "web logout");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /saiu -> "you are logged out" page (no authentication) */
static esp_err_t logged_out_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, SIMPLE_PAGE_HEAD "</head><body><div>"
                    "<h2>Você saiu</h2>"
                    "<p>Para voltar à configuração do gateway, entre de novo com usuário e senha.</p>"
                    "<form action='/' method='get'><button class='p'>Entrar novamente</button></form>"
                    "<p style='font-size:13px'>Se o navegador entrar direto sem pedir a senha, feche todas as janelas dele.</p>"
                    SIMPLE_PAGE_END, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /logs -> live log page */
static esp_err_t logs_page_handler(httpd_req_t *req)
{
    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, logs_page_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /logs_data?from=<offset> -> new log text since offset */
static esp_err_t logs_data_handler(httpd_req_t *req)
{
    static char buf[4096];
    char query[32], val[16], hdr[16], hdr2[16];
    uint32_t from = 0, start, next;
    size_t n;

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
            && httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK)
        from = strtoul(val, NULL, 10);
    n = weblog_read(from, buf, sizeof(buf), &start, &next);
    snprintf(hdr, sizeof(hdr), "%u", (unsigned)next);
    snprintf(hdr2, sizeof(hdr2), "%u", (unsigned)start);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Next", hdr);
    httpd_resp_set_hdr(req, "X-Log-Start", hdr2);
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* GET /logs_download -> whole RAM log as a .log file (ANSI colors removed) */
static esp_err_t logs_download_handler(httpd_req_t *req)
{
    static char in[1024], out[1024];
    char disp[112], stamp[24];
    const char *gw = (config[GW_ID].val && config[GW_ID].len) ? config[GW_ID].val : "gateway";
    uint32_t from = 0, start, next;
    int esc = 0;            /* 0 = text, 1 = got ESC, 2 = inside ESC[ ... */
    time_t now = time(NULL);
    struct tm tm_now;

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;

    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024)       /* clock set by NTP */
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);
    else
        snprintf(stamp, sizeof(stamp), "uptime%llus", (unsigned long long)(esp_timer_get_time() / 1000000));
    snprintf(disp, sizeof(disp), "attachment; filename=\"esp32_sx1302_%.16s_%s.log\"", gw, stamp);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    weblog_read(0, in, 0, &start, &next);    /* oldest offset still in RAM */
    from = start;
    for (;;) {
        size_t n = weblog_read(from, in, sizeof(in), &start, &next), o = 0;
        if (n == 0)
            break;
        from = next;
        for (size_t i = 0; i < n; i++) {
            char ch = in[i];
            if (esc == 1) { esc = (ch == '[') ? 2 : 0; continue; }
            if (esc == 2) { if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) esc = 0; continue; }
            if (ch == 0x1b) { esc = 1; continue; }
            if (ch == '\r') continue;
            out[o++] = ch;
        }
        if (o && httpd_resp_send_chunk(req, out, o) != ESP_OK)
            return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Network diagnostics (menu "Diagnósticos"), like OpenWrt Network > Diagnostics.
 * GET /diag     -> page (flash) + default host and suggestions (~300 B on stack)
 * GET /diag_run?t=ping|trace|dns&h=<host> -> result streamed line by line.
 * The HTTP server handles one request at a time, so only one test runs at once;
 * the raw ICMP socket exists only during the test (see diag.c).
 * ------------------------------------------------------------------------- */
static void diag_add_opt(char *b, size_t size, const char *v)
{
    char key[DIAG_HOST_MAX + 20];
    size_t n = strlen(b);

    if (v == NULL || !diag_host_ok(v))
        return;
    snprintf(key, sizeof(key), "value='%s'", v);
    if (strstr(b, key) == NULL && n + strlen(key) + 10 < size)
        snprintf(b + n, size - n, "<option %s>", key);
}

static esp_err_t diag_page_handler(httpd_req_t *req)
{
    char opts[320] = "", mqtt_host[DIAG_HOST_MAX + 1] = "", router[16] = "", inj[400];
    const char *ns = cfg_value(NS_HOST), *uri = cfg_value(MQTT_URI), *def;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;

    if (uri) {                                   /* mqtt[s]://[user@]host[:port][/...] */
        const char *p = strstr(uri, "://"), *at;
        size_t n;
        p = p ? p + 3 : uri;
        at = strchr(p, '@');
        if (at && at < p + strcspn(p, "/"))
            p = at + 1;
        n = strcspn(p, ":/?");
        if (n > 0 && n < sizeof(mqtt_host))
            memcpy(mqtt_host, p, n);
    }
    if (sta && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.gw.addr != 0)
        esp_ip4addr_ntoa(&ipi.gw, router, sizeof(router));

    diag_add_opt(opts, sizeof(opts), ns);                    /* LoRaWAN network server */
    diag_add_opt(opts, sizeof(opts), mqtt_host[0] ? mqtt_host : NULL);
    diag_add_opt(opts, sizeof(opts), router[0] ? router : NULL);  /* Wi-Fi router */
    diag_add_opt(opts, sizeof(opts), "8.8.8.8");
    diag_add_opt(opts, sizeof(opts), "openwrt.org");
    def = (ns && diag_host_ok(ns)) ? ns : router[0] ? router : "openwrt.org";
    snprintf(inj, sizeof(inj), "value='%s'><datalist id='sg'>%s</datalist>", def, opts);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, diag_page_a, sizeof(diag_page_a) - 1);
    httpd_resp_send_chunk(req, inj, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, diag_page_b, sizeof(diag_page_b) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static int diag_out(void *ctx, const char *line)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, line, HTTPD_RESP_USE_STRLEN) != ESP_OK;
}

static esp_err_t diag_run_handler(httpd_req_t *req)
{
    char query[112], t[8] = "", host[DIAG_HOST_MAX + 1] = "";
    bool ok = false;

    if (handle_basic_auth(req) == ESP_FAIL)
        return ESP_FAIL;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK
            && httpd_query_key_value(query, "t", t, sizeof(t)) == ESP_OK
            && httpd_query_key_value(query, "h", host, sizeof(host)) == ESP_OK) {
        char *r = host, *w = host;               /* %3A -> ':' (IPv6 literal) */
        for (; *r; r++, w++) {
            if (r[0] == '%' && (r[1] == '3') && (r[2] == 'A' || r[2] == 'a')) { *w = ':'; r += 2; }
            else *w = *r;
        }
        *w = '\0';
        ok = diag_host_ok(host) && (!strcmp(t, "ping") || !strcmp(t, "trace") || !strcmp(t, "dns"));
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "teste ou destino inválido\n", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "diag: %s %s", t, host);
    if (!strcmp(t, "ping"))
        diag_ping(host, DIAG_PING_MAX, diag_out, req);
    else if (!strcmp(t, "trace"))
        diag_traceroute(host, diag_out, req);
    else
        diag_nslookup(host, diag_out, req);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t diag_page_uri = {
    .uri       = "/diag",
    .method    = HTTP_GET,
    .handler   = diag_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t diag_run_uri = {
    .uri       = "/diag_run",
    .method    = HTTP_GET,
    .handler   = diag_run_handler,
    .user_ctx  = NULL
};

/* GET /favicon.svg and /favicon.ico -> gateway icon (antenna tower with radio waves).
 * No authentication, so the browser shows it also on the login prompt and on /saiu. */
static const char favicon_svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\"><rect width=\"32\" height=\"32\" rx=\"7\" fill=\"#0b7a5f\"/><g fill=\"none\" stroke=\"#fff\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M11.6 7.6a6.2 6.2 0 0 0 0 8.8M20.4 7.6a6.2 6.2 0 0 1 0 8.8\"/><path d=\"M7.9 4.2a11 11 0 0 0 0 15.6M24.1 4.2a11 11 0 0 1 0 15.6\" opacity=\".75\"/><path d=\"M12.4 27.5 16 14.5l3.6 13M13.5 23.5h5\"/></g><circle cx=\"16\" cy=\"12\" r=\"2.5\" fill=\"#fff\"/></svg>";

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_send(req, favicon_svg, sizeof(favicon_svg) - 1);
    return ESP_OK;
}

static const httpd_uri_t favicon_svg_uri = {
    .uri       = "/favicon.svg",
    .method    = HTTP_GET,
    .handler   = favicon_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t favicon_ico_uri = {
    .uri       = "/favicon.ico",
    .method    = HTTP_GET,
    .handler   = favicon_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t logs_download_uri = {
    .uri       = "/logs_download",
    .method    = HTTP_GET,
    .handler   = logs_download_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t logs_page_uri = {
    .uri       = "/logs",
    .method    = HTTP_GET,
    .handler   = logs_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t logs_data_uri = {
    .uri       = "/logs_data",
    .method    = HTTP_GET,
    .handler   = logs_data_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t config_json_uri = {
    .uri       = "/config.json",
    .method    = HTTP_GET,
    .handler   = config_json_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t mqtt_cert_uri = {
    .uri       = "/mqtt_cert",
    .method    = HTTP_POST,
    .handler   = mqtt_cert_handler,
    .user_ctx  = NULL
};

// Default: black theme. 'b' means 'black' background.
static const httpd_uri_t gw_config = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = gw_config_handler,
    .user_ctx  = "Hello " GW_NAME "!"
};

// light theme. 'w' means 'white' background
static const httpd_uri_t w_gw_config = {
    .uri       = "/w",
    .method    = HTTP_GET,
    .handler   = gw_config_handler,
    .user_ctx  = "Hello " GW_NAME "!"
};

// response
static const httpd_uri_t resp_config = {
    .uri       = "/resp",
    .method    = HTTP_POST,
    .handler   = gw_response_handler,
    .user_ctx  = "Response"
};

// reboot
static const httpd_uri_t reboot_config = {
    .uri       = "/reboot",
    .method    = HTTP_POST,
    .handler   = gw_reboot_handler,
    .user_ctx  = "ESP32 is rebooting"
};

// return au915 sub-band 0 json config
static const httpd_uri_t au915_sb0_json_conf = {
    .uri       = "/au915_sb0",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "au915_sb0"
};

// return au915 sub-band 1 json config
static const httpd_uri_t au915_sb1_json_conf = {
    .uri       = "/au915_sb1",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "au915_sb1"
};

// return cn470 json config
static const httpd_uri_t cn470_json_conf = {
    .uri       = "/cn470",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "cn470"
};

// return eu868 json config
static const httpd_uri_t eu868_json_conf = {
    .uri       = "/eu868",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "eu868"
};

// return us915 json config
static const httpd_uri_t us915_json_conf = {
    .uri       = "/us915",
    .method    = HTTP_GET,
    .handler   = gw_json_conf_handler,
    .user_ctx  = "us915"
};

static const httpd_uri_t logout_uri = {
    .uri       = "/logout",
    .method    = HTTP_GET,
    .handler   = logout_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t logged_out_uri = {
    .uri       = "/saiu",
    .method    = HTTP_GET,
    .handler   = logged_out_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;  // 21 handlers registered (default limit is 8), room for a few more

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");

        web_auth_apply();

        httpd_register_uri_handler(server, &gw_config);
        httpd_register_uri_handler(server, &w_gw_config);
        httpd_register_uri_handler(server, &resp_config);
        httpd_register_uri_handler(server, &reboot_config);
        httpd_register_uri_handler(server, &logs_page_uri);
        httpd_register_uri_handler(server, &logs_data_uri);
        httpd_register_uri_handler(server, &logs_download_uri);
        httpd_register_uri_handler(server, &diag_page_uri);
        httpd_register_uri_handler(server, &diag_run_uri);
        httpd_register_uri_handler(server, &logout_uri);
        httpd_register_uri_handler(server, &logged_out_uri);
        httpd_register_uri_handler(server, &favicon_svg_uri);
        httpd_register_uri_handler(server, &favicon_ico_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &config_json_uri);
        httpd_register_uri_handler(server, &mqtt_cert_uri);
        httpd_register_uri_handler(server, &au915_sb0_json_conf);
        httpd_register_uri_handler(server, &au915_sb1_json_conf);
        httpd_register_uri_handler(server, &cn470_json_conf);
        httpd_register_uri_handler(server, &eu868_json_conf);
        httpd_register_uri_handler(server, &us915_json_conf);

        return server;
    }

    ESP_LOGI(TAG, "Error starting web server!");
    return NULL;
}

void http_server_task(void *pvParameters)
{
    start_web_server();

    vTaskDelete(NULL);
}
