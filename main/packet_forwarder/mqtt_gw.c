/*
 * MQTT / MQTTS client of the gateway (see mqtt_gw.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "nvs.h"
#include "mqtt_client.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "mqtt_gw.h"
#include "web_config.h"
#include "branding.h"
#include "loragw_version.h"
#include "jitqueue.h"
#include "cs_codec.h"

#define CERT_NS  "mqtt_tls"             /* NVS namespace of the certificates */

static const char *TAG = "gw_mqtt";
extern config_s config[CONFIG_NUM];

static SemaphoreHandle_t s_lock;
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char *s_ca, *s_crt, *s_key;      /* PEM buffers used by the running client */
static char s_gwid[24];
static char s_gwid_lc[24];              /* lower case, as ChirpStack uses it in topics and JSON */
static char s_topic[96];                /* base topic */
static char s_state_topic[112];
static char s_down_topic[112];
static uint8_t s_lwt[64];               /* last will payload (text, JSON or Protobuf) */
static int s_lwt_len;
static bool s_cs;                       /* running client uses the ChirpStack format */
static bool s_pb;                       /* ChirpStack messages in Protobuf (else JSON) */
static uint32_t s_down_rx, s_down_ok;

/* ---- ChirpStack downlinks ----
 * A DownlinkFrame has 1..N alternatives (e.g. RX1 and RX2). The first one is
 * handed to the packet forwarder; if it is refused (too late, collision...)
 * the next one is tried, like the Gateway Bridge does. The result is
 * reported in <base>/event/ack. */
#define DOWN_MAX_ITEMS  CS_DOWN_ITEMS
#define DOWN_SLOTS      4
#define DOWN_JSON_MAX   600

typedef struct {
    uint32_t id;
    char json[DOWN_JSON_MAX];
} down_msg_t;

typedef struct {
    bool used;
    uint32_t id;
    int n, cur;
    char *txpk[DOWN_MAX_ITEMS];
    int status[DOWN_MAX_ITEMS];         /* enum cs_ack_status, -1 = not tried yet */
} down_slot_t;

static QueueHandle_t s_down_q;
static down_slot_t s_slots[DOWN_SLOTS];
static char s_last_err[96];
static uint32_t s_sent, s_dropped, s_connects;

static const char *cfgv(int tag)
{
    return (config[tag].val != NULL && config[tag].len > 0) ? config[tag].val : NULL;
}

static bool mqtt_enabled(void)
{
    const char *v = cfgv(MQTT_EN);
    return v != NULL && strcmp(v, "on") == 0;
}

/* "chirpstack" = ChirpStack v4 gateway format (replaces the UDP forwarder),
 * anything else = mirror of the Semtech JSON (UDP keeps working) */
static bool fmt_chirpstack(void)
{
    const char *v = cfgv(MQTT_FMT);
    return v != NULL && strcmp(v, "chirpstack") == 0;
}

/* ChirpStack encoding: "protobuf" or "json" (default) */
static bool enc_protobuf(void)
{
    const char *v = cfgv(MQTT_ENC);
    return v != NULL && strcmp(v, "protobuf") == 0;
}

/* UDP and MQTT are exclusive: MQTT enabled = UDP forwarder off */
bool mqtt_gw_udp_off(void)
{
    return mqtt_enabled();
}

/* ChirpStack v4 region id (topic prefix) of the selected frequency plan */
static const char *cs_region(void)
{
    const char *r = cfgv(FREQ_REGION);
    if (r == NULL || !strcmp(r, "au915_sb1")) return "au915_1";
    if (!strcmp(r, "au915_sb0")) return "au915_0";
    if (!strcmp(r, "us915"))     return "us915_0";
    if (!strcmp(r, "eu868"))     return "eu868";
    if (!strcmp(r, "cn470"))     return "cn470_10";
    return "au915_1";
}

static bool valid_cert_type(const char *t)
{
    return t && (!strcmp(t, "ca") || !strcmp(t, "crt") || !strcmp(t, "key"));
}

/* -------------------------------------------------------------------------- */
/* certificates in NVS                                                         */

static char *cert_load(const char *type)
{
    nvs_handle_t h;
    size_t len = 0;
    char *p = NULL;

    if (nvs_open(CERT_NS, NVS_READONLY, &h) != ESP_OK)
        return NULL;
    if (nvs_get_blob(h, type, NULL, &len) == ESP_OK && len > 1 && len <= MQTT_GW_CERT_MAX + 1) {
        p = malloc(len + 1);
        if (p && nvs_get_blob(h, type, p, &len) == ESP_OK) {
            p[len] = '\0';
        } else {
            free(p);
            p = NULL;
        }
    }
    nvs_close(h);
    return p;
}

static bool cert_present(const char *type)
{
    nvs_handle_t h;
    size_t len = 0;
    bool ok = false;

    if (nvs_open(CERT_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    ok = (nvs_get_blob(h, type, NULL, &len) == ESP_OK && len > 1);
    nvs_close(h);
    return ok;
}

esp_err_t mqtt_gw_cert_set(const char *type, const char *pem, size_t len)
{
    nvs_handle_t h;
    esp_err_t err;

    if (!valid_cert_type(type))
        return ESP_ERR_INVALID_ARG;
    if (len > MQTT_GW_CERT_MAX)
        return ESP_ERR_INVALID_SIZE;
    if (len > 0 && (strstr(pem, "-----BEGIN") == NULL || strstr(pem, "-----END") == NULL))
        return ESP_ERR_INVALID_ARG;   /* not a PEM */

    err = nvs_open(CERT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    if (len == 0) {
        err = nvs_erase_key(h, type);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
    } else {
        /* store with a terminating '\0' (mbedTLS needs it for PEM) */
        char *buf = malloc(len + 1);
        if (!buf) {
            nvs_close(h);
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf, pem, len);
        buf[len] = '\0';
        err = nvs_set_blob(h, type, buf, len + 1);
        free(buf);
    }
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "certificate '%s' %s: %s", type, len ? "saved" : "removed", esp_err_to_name(err));
    return err;
}

/* -------------------------------------------------------------------------- */
/* ChirpStack v4 format                                                        */

static SemaphoreHandle_t s_down_lock;
static int s_slot_rr;

static void slot_free(down_slot_t *sl)
{
    for (int i = 0; i < DOWN_MAX_ITEMS; i++) {
        free(sl->txpk[i]);
        sl->txpk[i] = NULL;
    }
    sl->used = false;
}

static void down_clear(void)
{
    if (!s_down_lock)
        return;
    xSemaphoreTake(s_down_lock, portMAX_DELAY);
    for (int i = 0; i < DOWN_SLOTS; i++)
        slot_free(&s_slots[i]);
    if (s_down_q)
        xQueueReset(s_down_q);
    xSemaphoreGive(s_down_lock);
}

static int jit_to_status(int e)
{
    switch (e) {
    case JIT_ERROR_OK:
    case JIT_ERROR_TX_POWER:          return CS_ACK_OK;   /* sent with the closest supported power */
    case JIT_ERROR_TOO_LATE:          return CS_ACK_TOO_LATE;
    case JIT_ERROR_TOO_EARLY:         return CS_ACK_TOO_EARLY;
    case JIT_ERROR_FULL:              return CS_ACK_QUEUE_FULL;
    case JIT_ERROR_COLLISION_PACKET:  return CS_ACK_COLLISION_PACKET;
    case JIT_ERROR_COLLISION_BEACON:  return CS_ACK_COLLISION_BEACON;
    case JIT_ERROR_TX_FREQ:           return CS_ACK_TX_FREQ;
    case JIT_ERROR_GPS_UNLOCKED:      return CS_ACK_GPS_UNLOCKED;
    default:                          return CS_ACK_INTERNAL_ERROR;
    }
}

/* publish <base>/<sub> with a ChirpStack message (JSON or Protobuf) */
static bool cs_publish(esp_mqtt_client_handle_t cl, const char *sub, const void *data, int len, int retain)
{
    char topic[128];
    if (!cl || len <= 0)
        return false;
    snprintf(topic, sizeof(topic), "%s/%s", s_topic, sub);
    return esp_mqtt_client_enqueue(cl, topic, (const char *)data, len, 1, retain, true) >= 0;
}

/* <base>/event/ack (caller holds s_down_lock) */
static void publish_ack(esp_mqtt_client_handle_t cl, down_slot_t *sl)
{
    uint8_t buf[320];
    int st[DOWN_MAX_ITEMS], n;

    for (int i = 0; i < sl->n; i++)
        st[i] = sl->status[i] < 0 ? CS_ACK_IGNORED : sl->status[i];
    n = s_pb ? cs_ack_pb(s_gwid_lc, sl->id, st, sl->n, buf, sizeof(buf))
             : cs_ack_json(s_gwid_lc, sl->id, st, sl->n, (char *)buf, sizeof(buf));
    cs_publish(cl, "event/ack", buf, n, 0);
}

/* hands the current alternative of the slot to the packet forwarder */
static void slot_send_current(down_slot_t *sl)
{
    static down_msg_t m;   /* only called with s_down_lock held */
    m.id = sl->id;
    snprintf(m.json, sizeof(m.json), "%s", sl->txpk[sl->cur]);
    if (xQueueSend(s_down_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "downlink queue full");
        sl->status[sl->cur] = CS_ACK_QUEUE_FULL;
    }
}

/* <base>/command/down received (runs in the MQTT task) */
static void down_frame(const char *data, int len)
{
    cs_down_frame_t *fr;
    down_slot_t *sl;
    bool is_json = false;
    int i;

    s_down_rx++;
    fr = malloc(sizeof(*fr));
    if (fr == NULL)
        return;
    if (cs_down_parse((const uint8_t *)data, len, fr, &is_json) != 0 || fr->n == 0) {
        ESP_LOGW(TAG, "downlink: invalid %s DownlinkFrame (%d bytes)", is_json ? "JSON" : "Protobuf", len);
        free(fr);
        return;
    }

    xSemaphoreTake(s_down_lock, portMAX_DELAY);
    sl = NULL;
    for (i = 0; i < DOWN_SLOTS; i++)
        if (!s_slots[i].used) { sl = &s_slots[i]; break; }
    if (sl == NULL) {                       /* reuse the oldest one */
        sl = &s_slots[s_slot_rr];
        s_slot_rr = (s_slot_rr + 1) % DOWN_SLOTS;
        slot_free(sl);
    }
    memset(sl, 0, sizeof(*sl));
    sl->used = true;
    sl->id = fr->id;
    sl->n = fr->n;
    for (i = 0; i < sl->n; i++) {
        char *buf = malloc(DOWN_JSON_MAX);
        const char *err = NULL;
        sl->status[i] = -1;
        if (buf && cs_item_txpk(&fr->items[i], buf, DOWN_JSON_MAX, &err) > 0) {
            sl->txpk[i] = buf;
        } else {
            free(buf);
            ESP_LOGW(TAG, "downlink %u item %d: %s", (unsigned)sl->id, i, err ? err : "no memory");
            sl->status[i] = CS_ACK_INTERNAL_ERROR;
        }
    }
    free(fr);

    /* first valid alternative */
    for (sl->cur = 0; sl->cur < sl->n && sl->txpk[sl->cur] == NULL; sl->cur++)
        ;
    if (sl->cur < sl->n) {
        ESP_LOGI(TAG, "downlink %u (%s, %d alternative%s) -> concentrator", (unsigned)sl->id,
                 is_json ? "JSON" : "Protobuf", sl->n, sl->n > 1 ? "s" : "");
        slot_send_current(sl);
    }
    if (sl->cur >= sl->n || sl->status[sl->cur] >= 0) {   /* nothing could be queued */
        publish_ack(s_client, sl);
        slot_free(sl);
    }
    xSemaphoreGive(s_down_lock);
}

int mqtt_gw_down_wait(char *buf, size_t size, uint32_t *id, int timeout_ms)
{
    static down_msg_t m;   /* only the downstream thread calls this */

    if (!s_down_q) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return -1;
    }
    if (xQueueReceive(s_down_q, &m, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return -1;
    *id = m.id;
    return snprintf(buf, size, "%s", m.json);
}

void mqtt_gw_down_result(uint32_t id, int jit_error)
{
    down_slot_t *sl = NULL;
    int st = jit_to_status(jit_error);

    if (!s_down_lock)
        return;
    xSemaphoreTake(s_down_lock, portMAX_DELAY);
    for (int i = 0; i < DOWN_SLOTS; i++)
        if (s_slots[i].used && s_slots[i].id == id) { sl = &s_slots[i]; break; }
    if (sl == NULL) {
        xSemaphoreGive(s_down_lock);
        return;
    }
    sl->status[sl->cur] = st;
    if (st != CS_ACK_OK) {
        /* try the next alternative (e.g. RX2 when RX1 is too late) */
        ESP_LOGW(TAG, "downlink %u item %d: %s", (unsigned)id, sl->cur, cs_ack_name(st));
        while (++sl->cur < sl->n) {
            if (sl->txpk[sl->cur]) {
                slot_send_current(sl);
                if (sl->status[sl->cur] < 0) {
                    xSemaphoreGive(s_down_lock);
                    return;
                }
            }
        }
    } else {
        s_down_ok++;
        ESP_LOGI(TAG, "downlink %u scheduled (item %d)", (unsigned)id, sl->cur);
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_connected)
            publish_ack(s_client, sl);
        xSemaphoreGive(s_lock);
    }
    slot_free(sl);
    xSemaphoreGive(s_down_lock);
}

/* Semtech PUSH_DATA JSON -> <base>/event/up (one per packet) and <base>/event/stats
 * (caller holds s_lock, client connected) */
static void publish_chirpstack(const char *json, int len)
{
    static const cs_meta_t meta[] = { { "gateway", GW_NAME }, { "version", ESP32_SX1302_VERSION } };
    JSON_Value *root;
    JSON_Object *o, *stat;
    JSON_Array *rx;
    uint8_t *buf;
    int n;
    const size_t BUFSZ = 1024;

    (void)len;
    root = json_parse_string(json);      /* the forwarder buffer is '\0' terminated */
    o = json_value_get_object(root);
    buf = malloc(BUFSZ + sizeof(cs_uplink_t));
    if (o == NULL || buf == NULL) {
        json_value_free(root);
        free(buf);
        s_dropped++;
        return;
    }
    rx = json_object_get_array(o, "rxpk");
    for (size_t i = 0; rx && i < json_array_get_count(rx); i++) {
        cs_uplink_t *u = (cs_uplink_t *)(buf + BUFSZ);
        n = -1;
        if (cs_parse_rxpk(json_array_get_object(rx, i), esp_random(), u) == 0)
            n = s_pb ? cs_uplink_pb(u, s_gwid_lc, buf, BUFSZ) : cs_uplink_json(u, s_gwid_lc, (char *)buf, BUFSZ);
        if (cs_publish(s_client, "event/up", buf, n, 0))
            s_sent++;
        else
            s_dropped++;
    }
    stat = json_object_get_object(o, "stat");
    if (stat) {
        cs_stats_t st;
        cs_parse_stat(stat, &st);
        n = s_pb ? cs_stats_pb(&st, s_gwid_lc, meta, 2, buf, BUFSZ)
                 : cs_stats_json(&st, s_gwid_lc, meta, 2, (char *)buf, BUFSZ);
        if (cs_publish(s_client, "event/stats", buf, n, 0))
            s_sent++;
        else
            s_dropped++;
    }
    free(buf);
    json_value_free(root);
}

/* -------------------------------------------------------------------------- */
/* client                                                                      */

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_connects++;
        s_last_err[0] = '\0';
        ESP_LOGI(TAG, "connected to the broker");
        if (s_cs) {
            uint8_t st[96];
            int n = s_pb ? cs_conn_pb(s_gwid_lc, true, st, sizeof(st)) : cs_conn_json(s_gwid_lc, true, (char *)st, sizeof(st));
            esp_mqtt_client_publish(event->client, s_state_topic, (const char *)st, n, 1, 1);
            esp_mqtt_client_subscribe(event->client, s_down_topic, 1);
            ESP_LOGI(TAG, "ChirpStack mode: waiting downlinks on %s", s_down_topic);
        } else {
            esp_mqtt_client_publish(event->client, s_state_topic, "online", 0, 1, 1);
        }
        break;
    case MQTT_EVENT_DATA:
        if (s_cs && event->topic_len == (int)strlen(s_down_topic)
                && strncmp(event->topic, s_down_topic, event->topic_len) == 0) {
            if (event->current_data_offset != 0 || event->data_len != event->total_data_len)
                ESP_LOGW(TAG, "downlink bigger than the MQTT buffer, ignored");
            else
                down_frame(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected)
            ESP_LOGW(TAG, "disconnected from the broker");
        s_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            if (event->error_handle->esp_tls_last_esp_err != 0)
                snprintf(s_last_err, sizeof(s_last_err), "TLS/TCP: %s (0x%x, tls 0x%x, cert 0x%x)",
                         esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                         event->error_handle->esp_tls_last_esp_err,
                         event->error_handle->esp_tls_stack_err,
                         event->error_handle->esp_tls_cert_verify_flags);
            else
                snprintf(s_last_err, sizeof(s_last_err), "TCP: errno %d",
                         event->error_handle->esp_transport_sock_errno);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            snprintf(s_last_err, sizeof(s_last_err), "broker refused the connection (code %d: user/password?)",
                     event->error_handle->connect_return_code);
        } else {
            snprintf(s_last_err, sizeof(s_last_err), "error type %d", event->error_handle->error_type);
        }
        ESP_LOGW(TAG, "%s", s_last_err);
        break;
    default:
        break;
    }
}

static void stop_locked(void)
{
    if (s_client) {
        if (s_connected)
            esp_mqtt_client_publish(s_client, s_state_topic, (const char *)s_lwt, s_lwt_len, 1, 1);
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    free(s_ca);  s_ca = NULL;
    free(s_crt); s_crt = NULL;
    free(s_key); s_key = NULL;
}

void mqtt_gw_init(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_down_lock)
        s_down_lock = xSemaphoreCreateMutex();
    if (!s_down_q)
        s_down_q = xQueueCreate(DOWN_SLOTS, sizeof(down_msg_t));
}

void mqtt_gw_stop(void)
{
    mqtt_gw_init();
    down_clear();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s_lock);
}

bool mqtt_gw_connected(void)
{
    return s_connected;
}

void mqtt_gw_start(void)
{
    const char *uri, *user, *pass, *topic;
    bool tls;

    mqtt_gw_init();
    down_clear();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_locked();

    if (!mqtt_enabled()) {
        ESP_LOGI(TAG, "MQTT disabled");
        xSemaphoreGive(s_lock);
        return;
    }
    uri = cfgv(MQTT_URI);
    if (uri == NULL) {
        snprintf(s_last_err, sizeof(s_last_err), "broker address not set");
        ESP_LOGW(TAG, "%s", s_last_err);
        xSemaphoreGive(s_lock);
        return;
    }

    snprintf(s_gwid, sizeof(s_gwid), "%s", cfgv(GW_ID) ? cfgv(GW_ID) : "unknown");
    for (int i = 0; ; i++) {
        s_gwid_lc[i] = (char)tolower((unsigned char)s_gwid[i]);
        if (s_gwid[i] == '\0')
            break;
    }
    s_cs = fmt_chirpstack();
    s_pb = s_cs && enc_protobuf();
    topic = cfgv(MQTT_TOPIC);
    if (topic)
        snprintf(s_topic, sizeof(s_topic), "%s", topic);
    else if (s_cs)
        snprintf(s_topic, sizeof(s_topic), "%s/gateway/%s", cs_region(), s_gwid_lc);
    else
        snprintf(s_topic, sizeof(s_topic), "gateway/%s", s_gwid);
    while (strlen(s_topic) > 0 && s_topic[strlen(s_topic) - 1] == '/')
        s_topic[strlen(s_topic) - 1] = '\0';
    if (s_cs) {
        snprintf(s_state_topic, sizeof(s_state_topic), "%s/state/conn", s_topic);
        snprintf(s_down_topic, sizeof(s_down_topic), "%s/command/down", s_topic);
        s_lwt_len = s_pb ? cs_conn_pb(s_gwid_lc, false, s_lwt, sizeof(s_lwt))
                         : cs_conn_json(s_gwid_lc, false, (char *)s_lwt, sizeof(s_lwt));
    } else {
        snprintf(s_state_topic, sizeof(s_state_topic), "%s/state", s_topic);
        s_down_topic[0] = '\0';
        s_lwt_len = snprintf((char *)s_lwt, sizeof(s_lwt), "offline");
    }

    tls = (strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0);
    s_ca  = cert_load("ca");
    s_crt = cert_load("crt");
    s_key = cert_load("key");

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = uri;
    if (tls) {
        if (s_ca) {
            cfg.broker.verification.certificate = s_ca;
        } else {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;  /* public CAs */
#endif
        }
        if (s_crt && s_key) {
            cfg.credentials.authentication.certificate = s_crt;   /* mutual TLS */
            cfg.credentials.authentication.key = s_key;
        } else if (s_crt || s_key) {
            ESP_LOGW(TAG, "client certificate and key must both be installed; mutual TLS not used");
        }
    }
    cfg.credentials.client_id = s_gwid;
    user = cfgv(MQTT_USER);
    if (user) {
        cfg.credentials.username = user;
        pass = cfgv(MQTT_PSWD);
        if (pass)
            cfg.credentials.authentication.password = pass;
    }
    cfg.session.keepalive = 30;
    cfg.session.last_will.topic = s_state_topic;
    cfg.session.last_will.msg = (const char *)s_lwt;
    cfg.session.last_will.msg_len = s_lwt_len;
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = 1;
    cfg.network.reconnect_timeout_ms = 10000;
    cfg.buffer.size = 2048;
    cfg.task.stack_size = 6144 + 2048;   /* JSON parsing of the downlinks */

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        snprintf(s_last_err, sizeof(s_last_err), "client init failed (invalid address?)");
        ESP_LOGE(TAG, "%s", s_last_err);
        stop_locked();
        xSemaphoreGive(s_lock);
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    s_last_err[0] = '\0';
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting to %s (%s format, TLS %s, CA %s, client cert %s, user %s), topic %s",
             uri, s_cs ? (s_pb ? "ChirpStack v4 Protobuf" : "ChirpStack v4 JSON") : "Semtech JSON", tls ? "on" : "off", s_ca ? "custom" : (tls ? "bundle" : "-"),
             (s_crt && s_key) ? "yes" : "no", user ? user : "-", s_topic);
    xSemaphoreGive(s_lock);
}

void mqtt_gw_publish_uplink(const char *json, int len)
{
    char topic[128];
    char *payload;
    int plen;
    const char *sub;

    if (!s_client || !s_connected || json == NULL || len < 2 || json[0] != '{')
        return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    if (!s_client || !s_connected) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_cs) {
        publish_chirpstack(json, len);
        xSemaphoreGive(s_lock);
        return;
    }
    sub = (strstr(json, "\"rxpk\"") != NULL) ? "up" : "stats";
    snprintf(topic, sizeof(topic), "%s/%s", s_topic, sub);

    /* {"gw":"<id>", + rest of the Semtech JSON */
    payload = malloc(len + 48);
    if (payload) {
        plen = snprintf(payload, len + 48, "{\"gw\":\"%s\",%.*s", s_gwid, len - 1, json + 1);
        if (esp_mqtt_client_enqueue(s_client, topic, payload, plen, 1, 0, true) >= 0)
            s_sent++;
        else
            s_dropped++;
        free(payload);
    } else {
        s_dropped++;
    }
    xSemaphoreGive(s_lock);
}

static void json_str(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (; src && *src && j + 2 < n; src++) {
        if (*src == '"' || *src == '\\')
            dst[j++] = '\\';
        dst[j++] = (*src < 0x20) ? ' ' : *src;
    }
    dst[j] = '\0';
}

int mqtt_gw_status_json(char *buf, size_t size)
{
    char uri[128], user[64], topic[100], err[120];

    json_str(uri, sizeof(uri), cfgv(MQTT_URI));
    json_str(user, sizeof(user), cfgv(MQTT_USER));
    json_str(topic, sizeof(topic), s_client ? s_topic : (cfgv(MQTT_TOPIC) ? cfgv(MQTT_TOPIC) : ""));
    json_str(err, sizeof(err), s_last_err);
    return snprintf(buf, size,
        "{\"enabled\":%s,\"format\":\"%s\",\"encoding\":\"%s\",\"down\":%u,\"down_ok\":%u,\"running\":%s,\"connected\":%s,\"uri\":\"%s\",\"user\":\"%s\",\"topic\":\"%s\","
        "\"ca\":%s,\"crt\":%s,\"key\":%s,\"sent\":%u,\"dropped\":%u,\"connects\":%u,\"error\":\"%s\"}",
        mqtt_enabled() ? "true" : "false", fmt_chirpstack() ? "chirpstack" : "semtech", enc_protobuf() ? "protobuf" : "json",
        (unsigned)s_down_rx, (unsigned)s_down_ok, s_client ? "true" : "false", s_connected ? "true" : "false",
        uri, user, topic,
        cert_present("ca") ? "true" : "false", cert_present("crt") ? "true" : "false",
        cert_present("key") ? "true" : "false",
        (unsigned)s_sent, (unsigned)s_dropped, (unsigned)s_connects, err);
}

/* -------------------------------------------------------------------------- */
/* console commands                                                            */

static void set_cfg(int tag, const char *val)
{
    char *p = strdup(val ? val : "");
    if (!p)
        return;
    free(config[tag].val);
    config[tag].val = p;
    config[tag].len = strlen(p);
}

static struct {
    struct arg_lit *on, *off, *clear_user, *clear_pass, *clear_topic;
    struct arg_str *uri, *user, *pass, *topic, *format, *encoding;
    struct arg_end *end;
} mqtt_args;

static void print_status(void)
{
    printf("MQTT     : %s, %s\n", mqtt_enabled() ? "enabled" : "disabled",
           s_connected ? "CONNECTED" : (s_client ? "connecting..." : "stopped"));
    printf("UDP      : %s\n", mqtt_enabled() ? "OFF (the gateway sends only by MQTT)" : "on (MQTT disabled)");
    printf("Broker   : %s\n", cfgv(MQTT_URI) ? cfgv(MQTT_URI) : "(not set)");
    printf("User     : %s   Password: %s\n", cfgv(MQTT_USER) ? cfgv(MQTT_USER) : "(none)",
           cfgv(MQTT_PSWD) ? "(set)" : "(none)");
    if (fmt_chirpstack())
        printf("Format   : chirpstack (ChirpStack v4 gateway protocol), encoding %s\n",
               enc_protobuf() ? "protobuf" : "json");
    else
        printf("Format   : semtech (Semtech JSON, uplinks and stats only, no downlinks)\n");
    if (fmt_chirpstack())
        printf("Topic    : %s/{event/up,event/stats,event/ack,state/conn,command/down}\n",
               s_client ? s_topic : (cfgv(MQTT_TOPIC) ? cfgv(MQTT_TOPIC) : "<region>/gateway/<gateway id>"));
    else
        printf("Topic    : %s/{up,stats,state}\n", s_client ? s_topic : (cfgv(MQTT_TOPIC) ? cfgv(MQTT_TOPIC) : "gateway/<gateway id>"));
    printf("Certs    : CA %s | client cert %s | client key %s\n",
           cert_present("ca") ? "installed" : "-", cert_present("crt") ? "installed" : "-",
           cert_present("key") ? "installed" : "-");
    printf("Messages : sent %u, dropped %u, connections %u, downlinks %u (%u scheduled)\n", (unsigned)s_sent,
           (unsigned)s_dropped, (unsigned)s_connects, (unsigned)s_down_rx, (unsigned)s_down_ok);
    if (s_last_err[0])
        printf("Last err : %s\n", s_last_err);
}

static int cmd_mqtt(int argc, char **argv)
{
    bool changed = false;
    int n = arg_parse(argc, argv, (void **)&mqtt_args);
    if (n != 0) {
        arg_print_errors(stderr, mqtt_args.end, argv[0]);
        return 1;
    }
    if (mqtt_args.on->count)          { set_cfg(MQTT_EN, "on");  changed = true; }
    if (mqtt_args.off->count)         { set_cfg(MQTT_EN, "off"); changed = true; }
    if (mqtt_args.uri->count)         { set_cfg(MQTT_URI, mqtt_args.uri->sval[0]); changed = true; }
    if (mqtt_args.user->count)        { set_cfg(MQTT_USER, mqtt_args.user->sval[0]); changed = true; }
    if (mqtt_args.pass->count)        { set_cfg(MQTT_PSWD, mqtt_args.pass->sval[0]); changed = true; }
    if (mqtt_args.topic->count)       { set_cfg(MQTT_TOPIC, mqtt_args.topic->sval[0]); changed = true; }
    if (mqtt_args.clear_user->count)  { set_cfg(MQTT_USER, ""); changed = true; }
    if (mqtt_args.clear_pass->count)  { set_cfg(MQTT_PSWD, ""); changed = true; }
    if (mqtt_args.clear_topic->count) { set_cfg(MQTT_TOPIC, ""); changed = true; }
    if (mqtt_args.format->count) {
        const char *f = mqtt_args.format->sval[0];
        if (strcmp(f, "chirpstack") != 0 && strcmp(f, "semtech") != 0) {
            printf("ERROR: --format must be chirpstack or semtech\n");
            return 1;
        }
        set_cfg(MQTT_FMT, f);
        changed = true;
    }
    if (mqtt_args.encoding->count) {
        const char *e = mqtt_args.encoding->sval[0];
        if (strcmp(e, "json") != 0 && strcmp(e, "protobuf") != 0) {
            printf("ERROR: --encoding must be json or protobuf\n");
            return 1;
        }
        set_cfg(MQTT_ENC, e);
        changed = true;
    }

    if (changed) {
        save_config();
        mqtt_gw_start();      /* apply now */
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    print_status();
    return 0;
}

static int cmd_mqtt_cert(int argc, char **argv)
{
    char line[160];
    char *pem;
    size_t len = 0;
    esp_err_t err;

    if (argc < 2 || !valid_cert_type(argv[1])) {
        printf("usage: mqtt_cert <ca|crt|key> [clear]\n"
               "  ca  = CA that signed the broker certificate\n"
               "  crt = client certificate (mutual TLS)\n"
               "  key = client private key (mutual TLS)\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[2], "clear") == 0) {
        err = mqtt_gw_cert_set(argv[1], "", 0);
        printf("%s: %s\n", argv[1], err == ESP_OK ? "removed" : esp_err_to_name(err));
        mqtt_gw_start();
        return err == ESP_OK ? 0 : 1;
    }

    pem = malloc(MQTT_GW_CERT_MAX + 1);
    if (!pem)
        return 1;
    printf("Paste the PEM now (it ends at the line starting with \"-----END\"):\n");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';
        if (l == 0 && len == 0)
            continue;
        if (len + l + 2 > MQTT_GW_CERT_MAX) {
            printf("ERROR: PEM bigger than %d bytes\n", MQTT_GW_CERT_MAX);
            free(pem);
            return 1;
        }
        memcpy(pem + len, line, l);
        len += l;
        pem[len++] = '\n';
        pem[len] = '\0';
        if (strncmp(line, "-----END", 8) == 0)
            break;
    }
    err = mqtt_gw_cert_set(argv[1], pem, len);
    free(pem);
    printf("%s: %s (%u bytes)\n", argv[1], err == ESP_OK ? "saved" : esp_err_to_name(err), (unsigned)len);
    if (err == ESP_OK)
        mqtt_gw_start();
    return err == ESP_OK ? 0 : 1;
}

void mqtt_gw_register_console(void)
{
    mqtt_args.on          = arg_lit0(NULL, "on", "enable MQTT (the UDP forwarder is switched off)");
    mqtt_args.off         = arg_lit0(NULL, "off", "disable MQTT (back to the UDP forwarder)");
    mqtt_args.uri         = arg_str0(NULL, "uri", "<uri>", "broker, e.g. mqtts://broker.example.com:8883");
    mqtt_args.user        = arg_str0(NULL, "user", "<user>", "username");
    mqtt_args.pass        = arg_str0(NULL, "pass", "<password>", "password");
    mqtt_args.topic       = arg_str0(NULL, "topic", "<topic>", "base topic (default <region>/gateway/<id> or gateway/<ID>)");
    mqtt_args.format      = arg_str0(NULL, "format", "<chirpstack|semtech>", "chirpstack = ChirpStack v4 gateway protocol; semtech = Semtech JSON (no downlinks)");
    mqtt_args.encoding    = arg_str0(NULL, "encoding", "<json|protobuf>", "ChirpStack messages in JSON or Protobuf");
    mqtt_args.clear_user  = arg_lit0(NULL, "clear-user", "remove the username");
    mqtt_args.clear_pass  = arg_lit0(NULL, "clear-pass", "remove the password");
    mqtt_args.clear_topic = arg_lit0(NULL, "clear-topic", "use the default topic");
    mqtt_args.end         = arg_end(6);

    const esp_console_cmd_t mqtt_cmd = {
        .command = "mqtt",
        .help = "Show or change the MQTT settings (changes are saved and applied immediately)",
        .hint = NULL,
        .func = &cmd_mqtt,
        .argtable = &mqtt_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mqtt_cmd));

    const esp_console_cmd_t cert_cmd = {
        .command = "mqtt_cert",
        .help = "Install (paste PEM) or remove a TLS certificate: mqtt_cert <ca|crt|key> [clear]",
        .hint = "<ca|crt|key> [clear]",
        .func = &cmd_mqtt_cert,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cert_cmd));
}
