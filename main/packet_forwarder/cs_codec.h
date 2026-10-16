/*
 * ChirpStack v4 gateway messages (the ones the ChirpStack Gateway Bridge
 * publishes), in JSON or in Protobuf, built from / converted to the Semtech
 * UDP JSON used by the packet forwarder (rxpk / stat / txpk).
 *
 *   UplinkFrame    <- Semtech "rxpk" element
 *   GatewayStats   <- Semtech "stat" object
 *   ConnState, DownlinkTxAck
 *   DownlinkFrame  -> Semtech {"txpk":{...}} (one per alternative)
 *
 * Protobuf is written / read by hand for these messages only (field numbers
 * of chirpstack/api/proto/gw/gw.proto and common/common.proto).
 * No ESP-IDF dependency (only parson and base64): unit tested on the host.
 */
#ifndef _CS_CODEC_H
#define _CS_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "parson.h"

#define CS_PHY_MAX      256
#define CS_DOWN_ITEMS   3

/* gw.TxAckStatus */
enum cs_ack_status {
    CS_ACK_IGNORED = 0, CS_ACK_OK, CS_ACK_TOO_LATE, CS_ACK_TOO_EARLY, CS_ACK_COLLISION_PACKET,
    CS_ACK_COLLISION_BEACON, CS_ACK_TX_FREQ, CS_ACK_TX_POWER, CS_ACK_GPS_UNLOCKED,
    CS_ACK_QUEUE_FULL, CS_ACK_INTERNAL_ERROR
};

typedef struct {
    uint8_t  phy[CS_PHY_MAX];
    int      phy_len;
    uint32_t freq_hz;
    bool     lora;            /* else FSK */
    uint32_t bw_hz, sf;       /* LoRa */
    int      cr;              /* gw.CodeRate, 0 = undefined */
    uint32_t fsk_dr;          /* FSK */
    uint32_t uplink_id;
    bool     has_time;
    int64_t  t_sec;           /* gw_time (UTC) */
    int32_t  t_nanos;
    int      rssi;
    float    snr;
    uint32_t chan, rfch;
    uint32_t tmst;            /* concentrator counter -> context */
    int      crc;             /* gw.CRCStatus: 0 none, 1 bad, 2 ok */
} cs_uplink_t;

typedef struct {
    bool     has_time;
    int64_t  t_sec;
    bool     has_loc;
    double   lat, lon, alt;
    uint32_t rxnb, rxok, dwnb, txnb;
    float    temp;
} cs_stats_t;

typedef struct {
    uint8_t  phy[CS_PHY_MAX];
    int      phy_len;
    uint32_t freq_hz;
    int32_t  power;
    bool     lora, fsk;
    uint32_t bw_hz, sf, preamble;
    int      cr;              /* gw.CodeRate */
    char     cr_legacy[8];
    bool     ipol, no_crc;
    uint32_t fsk_dr, fdev;
    int      timing;          /* 0 immediately, 1 delay, 2 gps epoch */
    int64_t  t_us;            /* delay or time since GPS epoch, in us */
    uint8_t  ctx[16];
    int      ctx_len;
} cs_down_item_t;

typedef struct {
    uint32_t id;
    int n;
    cs_down_item_t items[CS_DOWN_ITEMS];
} cs_down_frame_t;

typedef struct { const char *key, *value; } cs_meta_t;

/* ---- Semtech JSON -> structures ---- */
int cs_parse_rxpk(JSON_Object *rxpk, uint32_t uplink_id, cs_uplink_t *u);
int cs_parse_stat(JSON_Object *stat, cs_stats_t *s);

/* ---- encoders: return the length, or -1 (buffer too small / invalid) ---- */
int cs_uplink_json(const cs_uplink_t *u, const char *gwid, char *out, size_t size);
int cs_uplink_pb(const cs_uplink_t *u, const char *gwid, uint8_t *out, size_t size);
int cs_stats_json(const cs_stats_t *s, const char *gwid, const cs_meta_t *meta, int nmeta, char *out, size_t size);
int cs_stats_pb(const cs_stats_t *s, const char *gwid, const cs_meta_t *meta, int nmeta, uint8_t *out, size_t size);
int cs_conn_json(const char *gwid, bool online, char *out, size_t size);
int cs_conn_pb(const char *gwid, bool online, uint8_t *out, size_t size);
int cs_ack_json(const char *gwid, uint32_t id, const int *status, int n, char *out, size_t size);
int cs_ack_pb(const char *gwid, uint32_t id, const int *status, int n, uint8_t *out, size_t size);

/* ---- DownlinkFrame (JSON when it starts with '{', else Protobuf) ---- */
int cs_down_parse(const uint8_t *data, size_t len, cs_down_frame_t *f, bool *is_json);

/* one alternative -> Semtech {"txpk":{...}}; returns the length or -1 (err = reason) */
int cs_item_txpk(const cs_down_item_t *it, char *out, size_t size, const char **err);

const char *cs_ack_name(int status);

#endif
