/*
 * ChirpStack v4 gateway messages in JSON / Protobuf (see cs_codec.h).
 *
 * JSON follows the proto3 JSON mapping used by ChirpStack (camelCase names,
 * bytes in base64, enums as strings, Duration "1.5s", Timestamp RFC 3339).
 * ChirpStack v4 detects JSON when the payload contains "gatewayId" and then
 * answers that gateway in JSON; otherwise it decodes Protobuf.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cs_codec.h"
#include "base64.h"

/* ========================================================================== */
/* helpers                                                                     */

static const char *const CR_NAMES[] = { "CR_UNDEFINED", "CR_4_5", "CR_4_6", "CR_4_7", "CR_4_8" };
static const char *const CR_SEMTECH[] = { NULL, "4/5", "4/6", "4/7", "4/8" };
static const char *const ACK_NAMES[] = {
    "IGNORED", "OK", "TOO_LATE", "TOO_EARLY", "COLLISION_PACKET", "COLLISION_BEACON",
    "TX_FREQ", "TX_POWER", "GPS_UNLOCKED", "QUEUE_FULL", "INTERNAL_ERROR"
};

const char *cs_ack_name(int status)
{
    return (status >= 0 && status <= CS_ACK_INTERNAL_ERROR) ? ACK_NAMES[status] : "INTERNAL_ERROR";
}

static int codr_to_cr(const char *codr)
{
    if (codr == NULL)         return 0;
    if (!strcmp(codr, "4/5")) return 1;
    if (!strcmp(codr, "4/6") || !strcmp(codr, "2/3")) return 2;
    if (!strcmp(codr, "4/7")) return 3;
    if (!strcmp(codr, "4/8") || !strcmp(codr, "1/2")) return 4;
    return 0;
}

static int crname_to_cr(const char *name)
{
    for (int i = 1; name && i <= 4; i++)
        if (!strcmp(name, CR_NAMES[i]))
            return i;
    return 0;
}

/* days since 1970-01-01 of a civil date (H. Hinnant) */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int64_t to_epoch(int Y, int M, int D, int h, int mi, int s)
{
    return days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + mi * 60 + s;
}

/* RFC 3339, with microseconds when us >= 0 */
static void fmt_time(int64_t sec, long us, char *buf, size_t size)
{
    int y;
    unsigned m, d;
    int64_t days = sec / 86400, rem = sec % 86400;
    if (rem < 0) { rem += 86400; days--; }
    civil_from_days(days, &y, &m, &d);
    if (us >= 0)
        snprintf(buf, size, "%04d-%02u-%02uT%02d:%02d:%02d.%06ldZ", y, m, d,
                 (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60), us);
    else
        snprintf(buf, size, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d,
                 (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
}

/* ========================================================================== */
/* Semtech JSON -> structures                                                  */

int cs_parse_rxpk(JSON_Object *rxpk, uint32_t uplink_id, cs_uplink_t *u)
{
    const char *data = json_object_get_string(rxpk, "data");
    const char *modu = json_object_get_string(rxpk, "modu");
    const char *time = json_object_get_string(rxpk, "time");
    double st;

    memset(u, 0, sizeof(*u));
    if (data == NULL || modu == NULL || json_object_get_value(rxpk, "freq") == NULL)
        return -1;
    u->phy_len = b64_to_bin(data, strlen(data), u->phy, sizeof(u->phy));
    if (u->phy_len < 0)
        return -1;
    u->freq_hz = (uint32_t)llround(json_object_get_number(rxpk, "freq") * 1e6);
    if (!strcmp(modu, "LORA")) {
        unsigned sf = 0, bw = 0;
        const char *datr = json_object_get_string(rxpk, "datr");
        if (datr == NULL || sscanf(datr, "SF%uBW%u", &sf, &bw) != 2)
            return -1;
        u->lora = true;
        u->sf = sf;
        u->bw_hz = bw * 1000;
        u->cr = codr_to_cr(json_object_get_string(rxpk, "codr"));
    } else if (!strcmp(modu, "FSK")) {
        u->fsk_dr = (uint32_t)json_object_get_number(rxpk, "datr");
    } else {
        return -1;
    }
    u->uplink_id = uplink_id;
    if (time != NULL) {
        int Y, M, D, h, mi;
        double s;
        if (sscanf(time, "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &mi, &s) == 6 && Y >= 2000) {
            u->has_time = true;
            u->t_sec = to_epoch(Y, M, D, h, mi, (int)s);
            u->t_nanos = (int32_t)llround((s - floor(s)) * 1e6) * 1000;
            if (u->t_nanos >= 1000000000) u->t_nanos = 999999000;
        }
    }
    u->rssi = (int)lround(json_object_get_number(rxpk, "rssi"));
    u->snr = (float)json_object_get_number(rxpk, "lsnr");
    u->chan = (uint32_t)json_object_get_number(rxpk, "chan");
    u->rfch = (uint32_t)json_object_get_number(rxpk, "rfch");
    u->tmst = (uint32_t)json_object_get_number(rxpk, "tmst");
    st = json_object_get_number(rxpk, "stat");
    u->crc = st > 0 ? 2 : (st < 0 ? 1 : 0);
    return 0;
}

int cs_parse_stat(JSON_Object *stat, cs_stats_t *s)
{
    const char *t = json_object_get_string(stat, "time");
    int Y, M, D, h, mi, sec;

    memset(s, 0, sizeof(*s));
    /* Semtech: "2026-10-14 19:00:18 GMT" (also accept ISO 8601) */
    if (t && (sscanf(t, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &sec) == 6 ||
              sscanf(t, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) == 6) && Y >= 2020) {
        s->has_time = true;
        s->t_sec = to_epoch(Y, M, D, h, mi, sec);
    }
    if (json_object_get_value(stat, "lati") && json_object_get_value(stat, "long")) {
        s->lat = json_object_get_number(stat, "lati");
        s->lon = json_object_get_number(stat, "long");
        s->alt = json_object_get_number(stat, "alti");
        s->has_loc = (s->lat != 0.0 || s->lon != 0.0);
    }
    s->rxnb = (uint32_t)json_object_get_number(stat, "rxnb");
    s->rxok = (uint32_t)json_object_get_number(stat, "rxok");
    s->dwnb = (uint32_t)json_object_get_number(stat, "dwnb");
    s->txnb = (uint32_t)json_object_get_number(stat, "txnb");
    s->temp = (float)json_object_get_number(stat, "temp");
    return 0;
}

/* ========================================================================== */
/* JSON encoders                                                               */

#define APPEND(...) do { \
        int _r = snprintf(out + n, (n < size) ? size - n : 0, __VA_ARGS__); \
        if (_r < 0) return -1; \
        n += (size_t)_r; \
    } while (0)

static void tmst_ctx(uint32_t tmst, uint8_t ctx[4])
{
    ctx[0] = (uint8_t)(tmst >> 24); ctx[1] = (uint8_t)(tmst >> 16);
    ctx[2] = (uint8_t)(tmst >> 8);  ctx[3] = (uint8_t)tmst;
}

int cs_uplink_json(const cs_uplink_t *u, const char *gwid, char *out, size_t size)
{
    size_t n = 0;
    char b64[CS_PHY_MAX * 4 / 3 + 8], ctx64[12], tbuf[40];
    uint8_t ctx[4];

    if (bin_to_b64(u->phy, u->phy_len, b64, sizeof(b64)) < 0)
        return -1;
    APPEND("{\"phyPayload\":\"%s\",\"txInfo\":{\"frequency\":%u,\"modulation\":{", b64, (unsigned)u->freq_hz);
    if (u->lora) {
        APPEND("\"lora\":{\"bandwidth\":%u,\"spreadingFactor\":%u", (unsigned)u->bw_hz, (unsigned)u->sf);
        if (u->cr > 0 && u->cr <= 4)
            APPEND(",\"codeRate\":\"%s\"", CR_NAMES[u->cr]);
        APPEND("}");
    } else {
        APPEND("\"fsk\":{\"datarate\":%u}", (unsigned)u->fsk_dr);
    }
    APPEND("}},\"rxInfo\":{\"gatewayId\":\"%s\",\"uplinkId\":%u", gwid, (unsigned)u->uplink_id);
    if (u->has_time) {
        fmt_time(u->t_sec, u->t_nanos / 1000, tbuf, sizeof(tbuf));
        APPEND(",\"gwTime\":\"%s\"", tbuf);
    }
    tmst_ctx(u->tmst, ctx);
    if (bin_to_b64(ctx, 4, ctx64, sizeof(ctx64)) < 0)
        return -1;
    APPEND(",\"rssi\":%d,\"snr\":%.1f,\"channel\":%u,\"rfChain\":%u,\"context\":\"%s\",\"crcStatus\":\"%s\"}}",
           u->rssi, (double)u->snr, (unsigned)u->chan, (unsigned)u->rfch, ctx64,
           u->crc == 2 ? "CRC_OK" : (u->crc == 1 ? "BAD_CRC" : "NO_CRC"));
    return (n < size) ? (int)n : -1;
}

int cs_stats_json(const cs_stats_t *s, const char *gwid, const cs_meta_t *meta, int nmeta, char *out, size_t size)
{
    size_t n = 0;
    char tbuf[40];

    APPEND("{\"gatewayId\":\"%s\"", gwid);
    if (s->has_time) {
        fmt_time(s->t_sec, -1, tbuf, sizeof(tbuf));
        APPEND(",\"time\":\"%s\"", tbuf);
    }
    if (s->has_loc)
        APPEND(",\"location\":{\"latitude\":%.6f,\"longitude\":%.6f,\"altitude\":%.0f,\"source\":\"GPS\"}",
               s->lat, s->lon, s->alt);
    APPEND(",\"rxPacketsReceived\":%u,\"rxPacketsReceivedOk\":%u,\"txPacketsReceived\":%u,\"txPacketsEmitted\":%u",
           (unsigned)s->rxnb, (unsigned)s->rxok, (unsigned)s->dwnb, (unsigned)s->txnb);
    APPEND(",\"metadata\":{");
    for (int i = 0; i < nmeta; i++)
        APPEND("\"%s\":\"%s\",", meta[i].key, meta[i].value);   /* constant strings, no escaping needed */
    APPEND("\"temperature\":\"%.1f\"}}", (double)s->temp);
    return (n < size) ? (int)n : -1;
}

int cs_conn_json(const char *gwid, bool online, char *out, size_t size)
{
    int n = snprintf(out, size, "{\"gatewayId\":\"%s\",\"state\":\"%s\"}", gwid, online ? "ONLINE" : "OFFLINE");
    return (n > 0 && (size_t)n < size) ? n : -1;
}

int cs_ack_json(const char *gwid, uint32_t id, const int *status, int n_items, char *out, size_t size)
{
    size_t n = 0;
    APPEND("{\"gatewayId\":\"%s\",\"downlinkId\":%u,\"items\":[", gwid, (unsigned)id);
    for (int i = 0; i < n_items; i++)
        APPEND("%s{\"status\":\"%s\"}", i ? "," : "", cs_ack_name(status[i]));
    APPEND("]}");
    return (n < size) ? (int)n : -1;
}

/* ========================================================================== */
/* Protobuf writer                                                             */

typedef struct {
    uint8_t *b;
    size_t cap, n;
} pbw_t;

static void pw_put(pbw_t *w, const void *p, size_t len)
{
    if (w->n + len <= w->cap)
        memcpy(w->b + w->n, p, len);
    w->n += len;      /* keeps counting: overflow detected at the end */
}

static void pw_varint(pbw_t *w, uint64_t v)
{
    uint8_t c;
    do {
        c = v & 0x7F;
        v >>= 7;
        if (v)
            c |= 0x80;
        pw_put(w, &c, 1);
    } while (v);
}

static void pw_key(pbw_t *w, uint32_t field, uint32_t wt) { pw_varint(w, ((uint64_t)field << 3) | wt); }

static void pw_u32(pbw_t *w, uint32_t f, uint32_t v)
{
    if (v) { pw_key(w, f, 0); pw_varint(w, v); }
}

static void pw_i64(pbw_t *w, uint32_t f, int64_t v)
{
    if (v) { pw_key(w, f, 0); pw_varint(w, (uint64_t)v); }   /* int32/int64: two's complement, 10 bytes if < 0 */
}

static void pw_bytes(pbw_t *w, uint32_t f, const void *p, size_t len)
{
    if (len) { pw_key(w, f, 2); pw_varint(w, len); pw_put(w, p, len); }
}

static void pw_str(pbw_t *w, uint32_t f, const char *s) { if (s) pw_bytes(w, f, s, strlen(s)); }

/* embedded message: always written (presence matters, e.g. an empty ack item) */
static void pw_msg(pbw_t *w, uint32_t f, const pbw_t *m)
{
    pw_key(w, f, 2);
    pw_varint(w, m->n);
    pw_put(w, m->b, m->n);
}

static void pw_double(pbw_t *w, uint32_t f, double v)
{
    if (v != 0.0) { pw_key(w, f, 1); pw_put(w, &v, 8); }   /* little endian (ESP32 and x86) */
}

static void pw_float(pbw_t *w, uint32_t f, float v)
{
    if (v != 0.0f) { pw_key(w, f, 5); pw_put(w, &v, 4); }
}

#define PBW(name, sz) uint8_t name##_buf[sz]; pbw_t name = { name##_buf, sizeof(name##_buf), 0 }
#define PB_DONE(w)    (((w).n <= (w).cap) ? (int)(w).n : -1)

int cs_uplink_pb(const cs_uplink_t *u, const char *gwid, uint8_t *out, size_t size)
{
    pbw_t w = { out, size, 0 };
    PBW(mi, 32); PBW(mod, 40); PBW(tx, 56); PBW(ts, 24); PBW(rx, 128);
    uint8_t ctx[4];

    /* UplinkTxInfo { 1 frequency, 2 modulation { 3 lora | 4 fsk } } */
    if (u->lora) {
        pw_u32(&mi, 1, u->bw_hz);
        pw_u32(&mi, 2, u->sf);
        pw_u32(&mi, 5, (uint32_t)u->cr);
        pw_msg(&mod, 3, &mi);
    } else {
        pw_u32(&mi, 2, u->fsk_dr);
        pw_msg(&mod, 4, &mi);
    }
    pw_u32(&tx, 1, u->freq_hz);
    pw_msg(&tx, 2, &mod);

    /* UplinkRxInfo */
    pw_str(&rx, 1, gwid);
    pw_u32(&rx, 2, u->uplink_id);
    if (u->has_time) {
        pw_i64(&ts, 1, u->t_sec);
        pw_i64(&ts, 2, u->t_nanos);
        pw_msg(&rx, 3, &ts);
    }
    pw_i64(&rx, 6, u->rssi);
    pw_float(&rx, 7, u->snr);
    pw_u32(&rx, 8, u->chan);
    pw_u32(&rx, 9, u->rfch);
    tmst_ctx(u->tmst, ctx);
    pw_bytes(&rx, 13, ctx, 4);
    pw_u32(&rx, 16, (uint32_t)u->crc);

    if (PB_DONE(mi) < 0 || PB_DONE(mod) < 0 || PB_DONE(tx) < 0 || PB_DONE(ts) < 0 || PB_DONE(rx) < 0)
        return -1;
    /* UplinkFrame { 1 phy_payload, 4 tx_info, 5 rx_info } */
    pw_bytes(&w, 1, u->phy, u->phy_len);
    pw_msg(&w, 4, &tx);
    pw_msg(&w, 5, &rx);
    return PB_DONE(w);
}

static void pw_meta(pbw_t *w, uint32_t f, const char *k, const char *v)
{
    PBW(e, 96);
    pw_str(&e, 1, k);
    pw_str(&e, 2, v);
    if (e.n <= e.cap)
        pw_msg(w, f, &e);
}

int cs_stats_pb(const cs_stats_t *s, const char *gwid, const cs_meta_t *meta, int nmeta, uint8_t *out, size_t size)
{
    pbw_t w = { out, size, 0 };
    PBW(ts, 16); PBW(loc, 40);
    char temp[16];

    pw_str(&w, 17, gwid);
    if (s->has_time) {
        pw_i64(&ts, 1, s->t_sec);
        pw_msg(&w, 2, &ts);
    }
    if (s->has_loc) {
        pw_double(&loc, 1, s->lat);
        pw_double(&loc, 2, s->lon);
        pw_double(&loc, 3, s->alt);
        pw_u32(&loc, 4, 1);                 /* common.LocationSource GPS */
        pw_msg(&w, 3, &loc);
    }
    pw_u32(&w, 5, s->rxnb);
    pw_u32(&w, 6, s->rxok);
    pw_u32(&w, 7, s->dwnb);
    pw_u32(&w, 8, s->txnb);
    for (int i = 0; i < nmeta; i++)
        pw_meta(&w, 10, meta[i].key, meta[i].value);
    snprintf(temp, sizeof(temp), "%.1f", (double)s->temp);
    pw_meta(&w, 10, "temperature", temp);
    return PB_DONE(w);
}

int cs_conn_pb(const char *gwid, bool online, uint8_t *out, size_t size)
{
    pbw_t w = { out, size, 0 };
    pw_u32(&w, 2, online ? 1 : 0);         /* ConnState { 2 state, 3 gateway_id } */
    pw_str(&w, 3, gwid);
    return PB_DONE(w);
}

int cs_ack_pb(const char *gwid, uint32_t id, const int *status, int n_items, uint8_t *out, size_t size)
{
    pbw_t w = { out, size, 0 };
    pw_u32(&w, 2, id);                     /* DownlinkTxAck { 2 downlink_id, 5 items, 6 gateway_id } */
    for (int i = 0; i < n_items; i++) {
        PBW(it, 8);
        pw_u32(&it, 1, (uint32_t)status[i]);
        pw_msg(&w, 5, &it);
    }
    pw_str(&w, 6, gwid);
    return PB_DONE(w);
}

/* ========================================================================== */
/* Protobuf reader (DownlinkFrame)                                             */

typedef struct {
    const uint8_t *p, *e;
    bool err;
} pbr_t;

static uint64_t pr_varint(pbr_t *r)
{
    uint64_t v = 0;
    for (int s = 0; s < 64; s += 7) {
        if (r->p >= r->e) { r->err = true; return 0; }
        uint8_t c = *r->p++;
        v |= (uint64_t)(c & 0x7F) << s;
        if (!(c & 0x80))
            return v;
    }
    r->err = true;
    return 0;
}

static bool pr_key(pbr_t *r, uint32_t *f, uint32_t *wt)
{
    if (r->err || r->p >= r->e)
        return false;
    uint64_t k = pr_varint(r);
    *f = (uint32_t)(k >> 3);
    *wt = (uint32_t)(k & 7);
    return !r->err;
}

static pbr_t pr_len(pbr_t *r)
{
    pbr_t s = { NULL, NULL, true };
    uint64_t n = pr_varint(r);
    if (r->err || n > (uint64_t)(r->e - r->p)) {
        r->err = true;
        return s;
    }
    s.p = r->p;
    s.e = r->p + n;
    s.err = false;
    r->p += n;
    return s;
}

static void pr_skip(pbr_t *r, uint32_t wt)
{
    switch (wt) {
    case 0: pr_varint(r); break;
    case 1: if (r->e - r->p < 8) r->err = true; else r->p += 8; break;
    case 2: pr_len(r); break;
    case 5: if (r->e - r->p < 4) r->err = true; else r->p += 4; break;
    default: r->err = true; break;
    }
}

static int pr_copy(pbr_t *r, uint8_t *dst, int max)
{
    pbr_t s = pr_len(r);
    int n = (int)(s.e - s.p);
    if (r->err || n > max) {
        r->err = true;
        return 0;
    }
    memcpy(dst, s.p, n);
    return n;
}

/* google.protobuf.Duration -> us */
static int64_t pr_duration(pbr_t *r)
{
    pbr_t s = pr_len(r);
    int64_t sec = 0, nanos = 0;
    uint32_t f, wt;
    while (pr_key(&s, &f, &wt)) {
        if (f == 1 && wt == 0)      sec = (int64_t)pr_varint(&s);
        else if (f == 2 && wt == 0) nanos = (int32_t)pr_varint(&s);
        else pr_skip(&s, wt);
    }
    if (s.err) r->err = true;
    return sec * 1000000 + nanos / 1000;
}

static void pr_modulation(pbr_t *r, cs_down_item_t *it)
{
    pbr_t m = pr_len(r);
    uint32_t f, wt, g, wt2;
    while (pr_key(&m, &f, &wt)) {
        if ((f == 3 || f == 4) && wt == 2) {
            pbr_t x = pr_len(&m);
            if (f == 3) it->lora = true; else it->fsk = true;
            while (pr_key(&x, &g, &wt2)) {
                if (f == 3 && g == 1 && wt2 == 0)      it->bw_hz = (uint32_t)pr_varint(&x);
                else if (f == 3 && g == 2 && wt2 == 0) it->sf = (uint32_t)pr_varint(&x);
                else if (f == 3 && g == 3 && wt2 == 2) {
                    int l = pr_copy(&x, (uint8_t *)it->cr_legacy, sizeof(it->cr_legacy) - 1);
                    it->cr_legacy[l] = '\0';
                }
                else if (f == 3 && g == 4 && wt2 == 0) it->ipol = pr_varint(&x) != 0;
                else if (f == 3 && g == 5 && wt2 == 0) it->cr = (int)pr_varint(&x);
                else if (f == 3 && g == 6 && wt2 == 0) it->preamble = (uint32_t)pr_varint(&x);
                else if (f == 3 && g == 7 && wt2 == 0) it->no_crc = pr_varint(&x) != 0;
                else if (f == 4 && g == 1 && wt2 == 0) it->fdev = (uint32_t)pr_varint(&x);
                else if (f == 4 && g == 2 && wt2 == 0) it->fsk_dr = (uint32_t)pr_varint(&x);
                else pr_skip(&x, wt2);
            }
            if (x.err) m.err = true;
        } else {
            pr_skip(&m, wt);
        }
    }
    if (m.err) r->err = true;
}

static void pr_timing(pbr_t *r, cs_down_item_t *it)
{
    pbr_t t = pr_len(r);
    uint32_t f, wt, g, wt2;
    while (pr_key(&t, &f, &wt)) {
        if (f == 1 && wt == 2) {
            pr_len(&t);
            it->timing = 0;
        } else if ((f == 2 || f == 3) && wt == 2) {
            pbr_t x = pr_len(&t);
            it->timing = (f == 2) ? 1 : 2;
            while (pr_key(&x, &g, &wt2)) {
                if (g == 1 && wt2 == 2) it->t_us = pr_duration(&x);
                else pr_skip(&x, wt2);
            }
            if (x.err) t.err = true;
        } else {
            pr_skip(&t, wt);
        }
    }
    if (t.err) r->err = true;
}

static int down_parse_pb(const uint8_t *data, size_t len, cs_down_frame_t *fr)
{
    pbr_t r = { data, data + len, false };
    uint32_t f, wt, g, wt2, h, wt3;

    while (pr_key(&r, &f, &wt)) {
        if (f == 3 && wt == 0) {
            fr->id = (uint32_t)pr_varint(&r);
        } else if (f == 5 && wt == 2) {
            pbr_t item = pr_len(&r);
            cs_down_item_t *it = (fr->n < CS_DOWN_ITEMS) ? &fr->items[fr->n] : NULL;
            if (it == NULL)
                continue;
            fr->n++;
            while (pr_key(&item, &g, &wt2)) {
                if (g == 1 && wt2 == 2) {
                    it->phy_len = pr_copy(&item, it->phy, sizeof(it->phy));
                } else if (g == 3 && wt2 == 2) {
                    pbr_t tx = pr_len(&item);
                    while (pr_key(&tx, &h, &wt3)) {
                        if (h == 1 && wt3 == 0)      it->freq_hz = (uint32_t)pr_varint(&tx);
                        else if (h == 2 && wt3 == 0) it->power = (int32_t)pr_varint(&tx);
                        else if (h == 3 && wt3 == 2) pr_modulation(&tx, it);
                        else if (h == 6 && wt3 == 2) pr_timing(&tx, it);
                        else if (h == 7 && wt3 == 2) it->ctx_len = pr_copy(&tx, it->ctx, sizeof(it->ctx));
                        else pr_skip(&tx, wt3);
                    }
                    if (tx.err) item.err = true;
                } else {
                    pr_skip(&item, wt2);
                }
            }
            if (item.err) r.err = true;
        } else {
            pr_skip(&r, wt);
        }
    }
    return r.err ? -1 : 0;
}

/* ========================================================================== */
/* JSON reader (DownlinkFrame)                                                 */

static int duration_us(const char *d, int64_t *us)
{
    char *end;
    double v;
    if (d == NULL)
        return -1;
    v = strtod(d, &end);
    if (end == d || *end != 's')
        return -1;
    *us = llround(v * 1e6);
    return 0;
}

static int down_parse_json(const char *txt, cs_down_frame_t *fr)
{
    JSON_Value *root = json_parse_string(txt);
    JSON_Object *o = json_value_get_object(root);
    JSON_Array *items = json_object_get_array(o, "items");
    int ret = 0;

    if (o == NULL || items == NULL) {
        json_value_free(root);
        return -1;
    }
    fr->id = (uint32_t)json_object_get_number(o, "downlinkId");
    for (size_t i = 0; i < json_array_get_count(items) && fr->n < CS_DOWN_ITEMS; i++) {
        JSON_Object *item = json_array_get_object(items, i);
        JSON_Object *tx = json_object_get_object(item, "txInfo");
        JSON_Object *lora = json_object_dotget_object(tx, "modulation.lora");
        JSON_Object *fsk = json_object_dotget_object(tx, "modulation.fsk");
        JSON_Object *timing = json_object_get_object(tx, "timing");
        const char *phy = json_object_get_string(item, "phyPayload");
        const char *c64 = json_object_get_string(tx, "context");
        cs_down_item_t *it = &fr->items[fr->n++];

        if (phy)
            it->phy_len = b64_to_bin(phy, strlen(phy), it->phy, sizeof(it->phy));
        it->freq_hz = (uint32_t)json_object_get_number(tx, "frequency");
        it->power = (int32_t)json_object_get_number(tx, "power");
        if (lora) {
            const char *cr = json_object_get_string(lora, "codeRate");
            const char *crl = json_object_get_string(lora, "codeRateLegacy");
            it->lora = true;
            it->bw_hz = (uint32_t)json_object_get_number(lora, "bandwidth");
            it->sf = (uint32_t)json_object_get_number(lora, "spreadingFactor");
            it->cr = crname_to_cr(cr);
            if (crl)
                snprintf(it->cr_legacy, sizeof(it->cr_legacy), "%s", crl);
            it->ipol = json_object_get_boolean(lora, "polarizationInversion") == 1;
            it->preamble = (uint32_t)json_object_get_number(lora, "preamble");
            it->no_crc = json_object_get_boolean(lora, "noCrc") == 1;
        } else if (fsk) {
            it->fsk = true;
            it->fdev = (uint32_t)json_object_get_number(fsk, "frequencyDeviation");
            it->fsk_dr = (uint32_t)json_object_get_number(fsk, "datarate");
        }
        if (timing && json_object_get_object(timing, "delay")) {
            it->timing = 1;
            if (json_object_dotget_value(timing, "delay.delay") != NULL
                    && duration_us(json_object_dotget_string(timing, "delay.delay"), &it->t_us) != 0)
                ret = -1;
        } else if (timing && json_object_get_object(timing, "gpsEpoch")) {
            it->timing = 2;
            if (duration_us(json_object_dotget_string(timing, "gpsEpoch.timeSinceGpsEpoch"), &it->t_us) != 0)
                ret = -1;
        }
        if (c64) {
            int l = b64_to_bin(c64, strlen(c64), it->ctx, sizeof(it->ctx));
            it->ctx_len = l > 0 ? l : 0;
        }
    }
    json_value_free(root);
    return ret;
}

int cs_down_parse(const uint8_t *data, size_t len, cs_down_frame_t *f, bool *is_json)
{
    size_t i = 0;
    memset(f, 0, sizeof(*f));
    while (i < len && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t'))
        i++;
    *is_json = (i < len && data[i] == '{');
    if (*is_json) {
        char *txt = malloc(len + 1);
        int r;
        if (txt == NULL)
            return -1;
        memcpy(txt, data, len);
        txt[len] = '\0';
        r = down_parse_json(txt, f);
        free(txt);
        return r;
    }
    return down_parse_pb(data, len, f);
}

/* ========================================================================== */
/* DownlinkFrame item -> Semtech txpk                                          */

int cs_item_txpk(const cs_down_item_t *it, char *out, size_t size, const char **err)
{
    size_t n = 0;
    char b64[CS_PHY_MAX * 4 / 3 + 8];

    *err = "invalid item";
    if (it->phy_len <= 0 || it->phy_len > 255) {
        *err = "invalid phyPayload";
        return -1;
    }
    if (bin_to_b64(it->phy, it->phy_len, b64, sizeof(b64)) < 0)
        return -1;

    APPEND("{\"txpk\":{");
    if (it->timing == 1) {
        uint32_t tmst;
        if (it->ctx_len != 4) {
            *err = "delay timing without a 4-byte context";
            return -1;
        }
        tmst = ((uint32_t)it->ctx[0] << 24) | ((uint32_t)it->ctx[1] << 16) | ((uint32_t)it->ctx[2] << 8) | it->ctx[3];
        tmst += (uint32_t)it->t_us;
        APPEND("\"imme\":false,\"tmst\":%u", (unsigned)tmst);
    } else if (it->timing == 2) {
        APPEND("\"imme\":false,\"tmms\":%lld", (long long)(it->t_us / 1000));
    } else {
        APPEND("\"imme\":true");   /* "immediately" (Class C) */
    }
    APPEND(",\"freq\":%.6f,\"rfch\":0,\"powe\":%d", it->freq_hz / 1e6, (int)it->power);

    if (it->lora) {
        const char *codr = (it->cr >= 1 && it->cr <= 4) ? CR_SEMTECH[it->cr]
                         : (codr_to_cr(it->cr_legacy) ? it->cr_legacy : "4/5");
        APPEND(",\"modu\":\"LORA\",\"datr\":\"SF%uBW%u\",\"codr\":\"%s\",\"ipol\":%s",
               (unsigned)it->sf, (unsigned)(it->bw_hz / 1000), codr, it->ipol ? "true" : "false");
        if (it->preamble > 0)
            APPEND(",\"prea\":%u", (unsigned)it->preamble);
        if (it->no_crc)
            APPEND(",\"ncrc\":true");
    } else if (it->fsk) {
        APPEND(",\"modu\":\"FSK\",\"datr\":%u,\"fdev\":%u", (unsigned)it->fsk_dr, (unsigned)it->fdev);
    } else {
        *err = "unsupported modulation";
        return -1;
    }
    APPEND(",\"size\":%d,\"data\":\"%s\"}}", it->phy_len, b64);
    if (n >= size) {
        *err = "too big";
        return -1;
    }
    *err = NULL;
    return (int)n;
}
