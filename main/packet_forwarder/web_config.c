/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2023 Semtech

Description:
    Functions for processing configure settings

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


bool nvs_ready = false;
static const char *TAG = "esp32 web config";


config_s config[CONFIG_NUM] = {
    { WIFI_SSID, "wifi_ssid", NULL, 0 },
    { WIFI_PASSWORD, "wifi_pswd", NULL, 0 },
    { NS_HOST, "ns_host", NULL, 0 },
    { NS_PORT, "ns_port", NULL, 0 },
    { GW_ID, "gw_id", NULL, 0 },
    { WIFI_MODE, "wifi_mode", NULL, 0 },
    { FREQ_REGION, "freq_region", NULL, 0 },
    { FREQ_RADIO0, "freq_radio0", NULL, 0 },
    { FREQ_RADIO1, "freq_radio1", NULL, 0 },
    { NTP_SERVER, "ntp_server", NULL, 0 },
    { MQTT_EN, "mqtt_en", NULL, 0 },
    { MQTT_URI, "mqtt_uri", NULL, 0 },
    { MQTT_USER, "mqtt_user", NULL, 0 },
    { MQTT_PSWD, "mqtt_pswd", NULL, 0 },
    { MQTT_TOPIC, "mqtt_topic", NULL, 0 },
    { WEB_USER, "web_user", NULL, 0 },
    { WEB_PSWD, "web_pswd", NULL, 0 },
    { MQTT_FMT, "mqtt_fmt", NULL, 0 },
    { MQTT_ENC, "mqtt_enc", NULL, 0 },
    { IP_MODE, "ip_mode", NULL, 0 },
    { IP_ADDR, "ip_addr", NULL, 0 },
    { IP_MASK, "ip_mask", NULL, 0 },
    { IP_GW, "ip_gw", NULL, 0 },
    { IP_DNS1, "ip_dns1", NULL, 0 },
    { IP_DNS2, "ip_dns2", NULL, 0 },
};

tag_e name2tag(char *name)
{
    int len;

    for(int i = 0; i < CONFIG_NUM; i++) {
        len = strlen(config[i].name);
        /* exact name: "mqtt_en" must not match "mqtt_enc=..." */
        if(strncmp(name, config[i].name, len) == 0 && (name[len] == '=' || name[len] == '\0' || name[len] == '&')){
            return config[i].tag;
        }
    }
    return CONFIG_ERR;
}

esp_err_t init_config_storage(void)
{
    if(nvs_ready)  // already ready; no need to init again
        return ESP_OK;

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased; retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err == ESP_OK)
        nvs_ready = true;

    return err;
}

// transfer encoded str such as 'abc%2Bde' to 'abc+de'.
int decode_ascii(char *str, char *p)
{
    char val;
    char buf[3] = { 0, 0, 0};

    while(*str){
        if(*str != '%'){
            if(*str == '+'){
                *p++ = ' ';  // space is encoded to '+' instead of '%20'
            } else {
                *p++ = *str;
            }
            str++;
        } else {
            if(*(str+1) && *(str+2)){
                buf[0] = *(str+1);
                if((unsigned char)buf[0] >= '8'){
                    printf("Error: not a valid encoded ascii char. Maybe an UTF-8 char?\n");
                    break;
                }
                buf[1] = *(str+2);
                val = (char)strtol(buf, NULL, 16);
                *p++ = val;
            } else {
                printf("Error: not a valid encoded ascii string\n");
                break;
            }
            str += 3;
        }
    }
    *p = '\0';

    return 0;
}

int update_config(char *str, int len)
{
    int n, name_len;
    char *p = NULL;

    tag_e tag = name2tag(str);
    if(tag < CONFIG_NUM){
        name_len = strlen(config[tag].name);
        n = len - name_len; // the space for '=' is saved used for '\0'
        if(n == 1) { // no value
            /* MQTT user / topic, radio frequencies and static IP fields are shown pre-filled:
             * an empty field means "clear it" */
            if(tag == MQTT_USER || tag == MQTT_TOPIC || tag == FREQ_RADIO0 || tag == FREQ_RADIO1
                    || (tag >= IP_ADDR && tag <= IP_DNS2)) {
                p = malloc(1);
                if(!p)
                    return -1;
                p[0] = '\0';
                if(config[tag].val)
                    free(config[tag].val);
                config[tag].val = p;
                config[tag].len = 0;
            }
            return 0;   // other fields: keep the old value
        }
        p = malloc(n);
        if(!p)
            return -1;

        if(config[tag].val)
            free(config[tag].val);
        strncpy(p, str + name_len + 1, n - 1);
        p[n-1] = '\0';
        if(tag == WIFI_SSID || tag == WIFI_PASSWORD || tag == NS_HOST || tag == NTP_SERVER
                || tag == MQTT_URI || tag == MQTT_USER || tag == MQTT_PSWD || tag == MQTT_TOPIC
                || tag == WEB_USER || tag == WEB_PSWD){
            decode_ascii(p, p);
            n = strlen(p) + 1;
        }
        config[tag].val = p;
        config[tag].len = n - 1;
    }
    return 0;
}

void dump_config(void)
{
    for(int i = 0; i < CONFIG_NUM; i++) {
        if(i == WIFI_PASSWORD || i == MQTT_PSWD || i == WEB_PSWD)  // don't display passwords
            continue;
        if(config[i].val != NULL)
            printf("%s: %s @ %p\n", config[i].name, config[i].val, config[i].val);
        else
            printf("%s: (no value)\n", config[i].name);
    }
}

esp_err_t read_config(void)
{
    nvs_handle_t my_handle;
    size_t len;
    char *p = NULL;

    if(nvs_ready != true){
        ESP_LOGW(TAG, "NVS storage not available");
        return ESP_FAIL;
    }

    esp_err_t err = nvs_open("nvs", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "opening NVS handle failed: %s!", esp_err_to_name(err));
        ESP_LOGW(TAG, "Config not updated");
        return ESP_FAIL;
    }

    for(int i = 0; i < CONFIG_NUM; i++){
        // get length and malloc space
        err = nvs_get_str(my_handle, config[i].name, NULL, &len);
        if(err != ESP_OK){
            printf("Error (%s) reading %s!\n", esp_err_to_name(err), config[i].name);
            config[i].val = NULL;
            config[i].len = 0;
            continue;
        }

        p = malloc(len);
        if(!p) {
            printf("Warning: malloc() for %u bytes failed\n", len);
            continue;  // just ignore if failed
        }

        // get value and save to config list
        err = nvs_get_str(my_handle, config[i].name, p, &len);
        if(err != ESP_OK){
            printf("Error (%s) reading %s!\n", esp_err_to_name(err), config[i].name);
            continue;
        }
        p[len-1] = '\0';
        config[i].val = p;
        config[i].len = len - 1;
    }
    nvs_close(my_handle);
    return ESP_OK;
}

int save_config(void)
{
    if(nvs_ready != true){
        ESP_LOGW(TAG, "NVS storage not available");
        return ESP_FAIL;
    }

    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("nvs", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "opening NVS handle failed: %s!", esp_err_to_name(err));
        ESP_LOGW(TAG, "Config not updated");
        return ESP_FAIL;
    }

    // Update config
    for(int i = 0; i < CONFIG_NUM; i++) {
        if(config[i].val != NULL){
            err = nvs_set_str(my_handle, config[i].name, config[i].val);
            printf("set %s: %s\n", config[i].name, (err != ESP_OK) ? "Failed!" : "Done");
        }
    }

    // Commit config
    printf("Save config to NVS... ");
    err = nvs_commit(my_handle);
    printf((err != ESP_OK) ? "Failed!\n" : "Done\n");
    nvs_close(my_handle);
    return ESP_OK;
}

void extract_data_items(char *str)
{
    char *start = str;

    if(str == NULL)
        return;

    while(*str != '\0'){
        if(*str == '&'){
            update_config(start, str-start);
            start = str+1;
        }
        str++;
    }
    update_config(start, str-start);
    printf("\n");
}

static int _config_wifi_mode(wifi_mode_e mode)
{
    int len = 7;
    char *p;

    p = malloc(len+1);
    if(!p) {
        printf("Warning: malloc() for %u bytes failed\n", len);
        return -1;
    }
    if(mode == WIFI_MODE_SOFT_AP)
        strncpy(p, "soft_ap", len+1);
    if(mode == WIFI_MODE_STATION)
        strncpy(p, "station", len+1);
    p[len] = '\0';
    printf("config wifi_mode = %s\n", p);

    if(config[WIFI_MODE].val)
        free(config[WIFI_MODE].val);
    config[WIFI_MODE].val = p;
    config[WIFI_MODE].len = len;

    save_config();
    return 0;
}

int config_wifi_mode(wifi_mode_e mode)
{
    if(mode == WIFI_MODE_SOFT_AP)
        return _config_wifi_mode(WIFI_MODE_SOFT_AP);

    if(mode == WIFI_MODE_STATION)
        return _config_wifi_mode(WIFI_MODE_STATION);

    return -1;
}

/*
int main(int argc, char *argv[])
{
    char buf[128] = "ns_host=192.168.1.232&wifi_ssid=mywifi&wifi_pswd=mypswd&ns_port=1680&apply=";

    extract_data_items(buf);
    dump_config();

    int n = save_config();
    read_config(conf_buf, n);
    dump_config();

    return 0;
}
*/

/* -------------------------------------------------------------------------- */
/* console: same settings as the web page, saved in the same NVS keys, so a    */
/* setting made by the browser or by a command is applied the same way at boot */

static const struct {
    const char *name;
    unsigned long r0, r1, min_mhz, max_mhz;
} regions[] = {
    { "au915_sb0", 915600000, 916300000, 915, 928 },
    { "au915_sb1", 917200000, 917900000, 915, 928 },
    { "us915",     904300000, 905000000, 902, 928 },
    { "eu868",     867500000, 868500000, 863, 870 },
    { "cn470",     470600000, 471400000, 470, 510 },
};
#define NREGIONS (sizeof(regions) / sizeof(regions[0]))

static int region_idx(const char *r)
{
    for (int i = 0; r && i < (int)NREGIONS; i++)
        if (strcmp(r, regions[i].name) == 0)
            return i;
    return -1;
}

static void cfg_set(int tag, const char *v)
{
    char *p = strdup(v ? v : "");
    if (!p)
        return;
    free(config[tag].val);
    config[tag].val = p;
    config[tag].len = strlen(p);
}

static const char *cfg_show(int tag, const char *def)
{
    return (config[tag].val && config[tag].len) ? config[tag].val : def;
}

static struct {
    struct arg_str *region, *radio0, *radio1, *ntp, *mode, *ssid, *wpass, *host, *gwid;
    struct arg_str *ipmode, *ip, *mask, *gw, *dns1, *dns2;
    struct arg_int *port;
    struct arg_lit *reboot;
    struct arg_end *end;
} cfg_args;

static void config_print(void)
{
    int ri = region_idx(cfg_show(FREQ_REGION, "au915_sb1"));
    printf("Saved settings (same as the web page; applied at boot):\n");
    printf("  Wi-Fi mode at boot : %s\n", cfg_show(WIFI_MODE, "soft_ap"));
    printf("  Wi-Fi SSID         : %s   password: %s\n", cfg_show(WIFI_SSID, "(not set)"),
           cfg_show(WIFI_PASSWORD, NULL) ? "(set)" : "(not set)");
    if (config[IP_MODE].val && strcmp(config[IP_MODE].val, "static") == 0)
        printf("  IP address         : static %s / %s  gateway %s  DNS %s %s\n", cfg_show(IP_ADDR, "(not set)"),
               cfg_show(IP_MASK, "(not set)"), cfg_show(IP_GW, "(not set)"),
               cfg_show(IP_DNS1, "(= gateway)"), cfg_show(IP_DNS2, ""));
    else
        printf("  IP address         : DHCP (automatic)\n");
    printf("  UDP server         : %s : %s\n", cfg_show(NS_HOST, "(not set)"), cfg_show(NS_PORT, "(not set)"));
    printf("  Gateway ID         : %s\n", cfg_show(GW_ID, "(from the MAC address)"));
    printf("  NTP server         : %s\n", cfg_show(NTP_SERVER, NTP_SERVER_ADDR));
    printf("  Frequency plan     : %s\n", cfg_show(FREQ_REGION, "au915_sb1 (default)"));
    printf("  Radio 0 / 1 (Hz)   : %s / %s\n",
           cfg_show(FREQ_RADIO0, ri >= 0 ? "(plan default)" : "-"), cfg_show(FREQ_RADIO1, ri >= 0 ? "(plan default)" : "-"));
    printf("  MQTT               : %s (format %s, encoding %s) -> UDP %s\n", cfg_show(MQTT_EN, "off"),
           cfg_show(MQTT_FMT, "semtech"), cfg_show(MQTT_ENC, "json"),
           (config[MQTT_EN].val && strcmp(config[MQTT_EN].val, "on") == 0) ? "OFF" : "on");
    printf("  Web login user     : %s\n", cfg_show(WEB_USER, "admin"));
    printf("Use 'mqtt' and 'web_auth' for MQTT and the web login. Changes of this command need a reboot\n"
           "('config --reboot' or 'reboot').\n");
}

static bool valid_freq(const char *v, int ri)
{
    unsigned long f;
    char *end;
    if (strlen(v) != 9)
        return false;
    f = strtoul(v, &end, 10);
    if (*end != '\0')
        return false;
    return ri < 0 || (f >= regions[ri].min_mhz * 1000000UL && f <= regions[ri].max_mhz * 1000000UL);
}

static int cmd_config(int argc, char **argv)
{
    bool changed = false;
    int ri;

    if (arg_parse(argc, argv, (void **)&cfg_args) != 0) {
        arg_print_errors(stderr, cfg_args.end, argv[0]);
        return 1;
    }
    if (cfg_args.region->count) {
        if (region_idx(cfg_args.region->sval[0]) < 0) {
            printf("ERROR: --region must be au915_sb0, au915_sb1, us915, eu868 or cn470\n");
            return 1;
        }
        cfg_set(FREQ_REGION, cfg_args.region->sval[0]);
        changed = true;
    }
    ri = region_idx(cfg_show(FREQ_REGION, "au915_sb1"));
    for (int i = 0; i < 2; i++) {
        struct arg_str *a = i ? cfg_args.radio1 : cfg_args.radio0;
        if (!a->count)
            continue;
        if (strcmp(a->sval[0], "default") == 0 || strcmp(a->sval[0], "0") == 0) {
            cfg_set(i ? FREQ_RADIO1 : FREQ_RADIO0, "");
        } else if (valid_freq(a->sval[0], ri)) {
            cfg_set(i ? FREQ_RADIO1 : FREQ_RADIO0, a->sval[0]);
        } else {
            printf("ERROR: --radio%d: 9 digits in Hz, between %lu and %lu MHz for %s (or 'default')\n", i,
                   ri >= 0 ? regions[ri].min_mhz : 0, ri >= 0 ? regions[ri].max_mhz : 0, ri >= 0 ? regions[ri].name : "?");
            return 1;
        }
        changed = true;
    }
    if (cfg_args.ntp->count)  { cfg_set(NTP_SERVER, cfg_args.ntp->sval[0]); changed = true; }
    if (cfg_args.mode->count) {
        const char *m = cfg_args.mode->sval[0];
        if (strcmp(m, "station") != 0 && strcmp(m, "soft_ap") != 0) {
            printf("ERROR: --mode must be station or soft_ap\n");
            return 1;
        }
        cfg_set(WIFI_MODE, m);
        changed = true;
    }
    if (cfg_args.ssid->count)  { cfg_set(WIFI_SSID, cfg_args.ssid->sval[0]); changed = true; }
    if (cfg_args.wpass->count) {
        if (strlen(cfg_args.wpass->sval[0]) < 8) {
            printf("ERROR: Wi-Fi password: at least 8 characters\n");
            return 1;
        }
        cfg_set(WIFI_PASSWORD, cfg_args.wpass->sval[0]);
        changed = true;
    }
    if (cfg_args.host->count)  { cfg_set(NS_HOST, cfg_args.host->sval[0]); changed = true; }
    if (cfg_args.port->count) {
        char b[8];
        if (cfg_args.port->ival[0] < 1 || cfg_args.port->ival[0] > 65535) {
            printf("ERROR: --port: 1 to 65535\n");
            return 1;
        }
        snprintf(b, sizeof(b), "%d", cfg_args.port->ival[0]);
        cfg_set(NS_PORT, b);
        changed = true;
    }
    if (cfg_args.gwid->count) {
        const char *g = cfg_args.gwid->sval[0];
        char up[17];
        if (strlen(g) != 16 || strspn(g, "0123456789abcdefABCDEF") != 16) {
            printf("ERROR: --gwid: 16 hexadecimal characters\n");
            return 1;
        }
        for (int i = 0; i < 16; i++)
            up[i] = (g[i] >= 'a' && g[i] <= 'f') ? g[i] - 32 : g[i];
        up[16] = '\0';
        cfg_set(GW_ID, up);
        changed = true;
    }

    if (cfg_args.ipmode->count) {
        const char *m = cfg_args.ipmode->sval[0];
        if (strcmp(m, "dhcp") != 0 && strcmp(m, "static") != 0) {
            printf("ERROR: --ip-mode must be dhcp or static\n");
            return 1;
        }
        cfg_set(IP_MODE, m);
        changed = true;
    }
    {
        struct arg_str *a[] = { cfg_args.ip, cfg_args.mask, cfg_args.gw, cfg_args.dns1, cfg_args.dns2 };
        const char *opt[] = { "ip", "mask", "gw", "dns1", "dns2" };
        for (int i = 0; i < 5; i++) {
            esp_ip4_addr_t t;
            if (!a[i]->count)
                continue;
            if (strcmp(a[i]->sval[0], "none") == 0) {
                cfg_set(IP_ADDR + i, "");
            } else if (esp_netif_str_to_ip4(a[i]->sval[0], &t) == ESP_OK) {
                cfg_set(IP_ADDR + i, a[i]->sval[0]);
            } else {
                printf("ERROR: --%s: IPv4 address like 192.168.1.50 (or 'none')\n", opt[i]);
                return 1;
            }
            changed = true;
        }
    }
    if (config[IP_MODE].val && strcmp(config[IP_MODE].val, "static") == 0
            && (!cfg_show(IP_ADDR, NULL) || !cfg_show(IP_MASK, NULL) || !cfg_show(IP_GW, NULL)))
        printf("WARNING: static IP needs --ip, --mask and --gw; until then DHCP is used\n");

    if (changed) {
        save_config();
        printf("Saved.\n");
    }
    config_print();
    if (cfg_args.reboot->count) {
        printf("Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

void config_register_console(void)
{
    cfg_args.region = arg_str0(NULL, "region", "<plan>", "au915_sb0 | au915_sb1 | us915 | eu868 | cn470");
    cfg_args.radio0 = arg_str0(NULL, "radio0", "<Hz|default>", "radio 0 center frequency, 9 digits");
    cfg_args.radio1 = arg_str0(NULL, "radio1", "<Hz|default>", "radio 1 center frequency, 9 digits");
    cfg_args.ntp    = arg_str0(NULL, "ntp", "<server>", "NTP server");
    cfg_args.mode   = arg_str0(NULL, "mode", "<station|soft_ap>", "Wi-Fi mode at the next boot");
    cfg_args.ssid   = arg_str0(NULL, "ssid", "<ssid>", "Wi-Fi network");
    cfg_args.wpass  = arg_str0(NULL, "wifi-pass", "<password>", "Wi-Fi password (min. 8)");
    cfg_args.host   = arg_str0(NULL, "host", "<host>", "UDP server (Gateway Bridge) host");
    cfg_args.port   = arg_int0(NULL, "port", "<port>", "UDP server port");
    cfg_args.gwid   = arg_str0(NULL, "gwid", "<16 hex>", "Gateway ID");
    cfg_args.ipmode = arg_str0(NULL, "ip-mode", "<dhcp|static>", "station IP address: automatic or static");
    cfg_args.ip     = arg_str0(NULL, "ip", "<a.b.c.d|none>", "static IP address");
    cfg_args.mask   = arg_str0(NULL, "mask", "<a.b.c.d|none>", "static netmask, e.g. 255.255.255.0");
    cfg_args.gw     = arg_str0(NULL, "gw", "<a.b.c.d|none>", "static default gateway (router)");
    cfg_args.dns1   = arg_str0(NULL, "dns1", "<a.b.c.d|none>", "static main DNS (none = the gateway)");
    cfg_args.dns2   = arg_str0(NULL, "dns2", "<a.b.c.d|none>", "static backup DNS");
    cfg_args.reboot = arg_lit0(NULL, "reboot", "reboot after saving");
    cfg_args.end    = arg_end(6);

    const esp_console_cmd_t cfg_cmd = {
        .command = "config",
        .help = "Show or change the saved settings (the same ones of the web page). Applied at boot",
        .hint = NULL,
        .func = &cmd_config,
        .argtable = &cfg_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cfg_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the gateway (applies the saved settings)",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));
}
