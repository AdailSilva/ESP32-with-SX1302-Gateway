/*
 * MQTT / MQTTS client of the gateway.
 *
 * Sends the gateway traffic to an MQTT broker, optionally over TLS with a CA
 * certificate and/or a client certificate (mutual TLS).
 *
 * Two formats (MQTT_FMT config):
 *
 * UDP and MQTT are exclusive: when MQTT is enabled the UDP forwarder is off.
 *
 * "semtech" (default) - Semtech JSON of the UDP protocol (uplinks and stats, no downlinks).
 *   base = MQTT_TOPIC, default "gateway/<gateway id>"
 *   <base>/up     uplink packets   {"gw":"<id>","rxpk":[...]}   (QoS 1)
 *   <base>/stats  gateway stats    {"gw":"<id>","stat":{...}}   (QoS 1)
 *   <base>/state  "online" / "offline" (retained, last will)
 *
 * "chirpstack" - ChirpStack v4 gateway protocol in JSON (what the ChirpStack
 *   Gateway Bridge publishes), JSON or Protobuf, with downlinks.
 *   base = MQTT_TOPIC, default "<region>/gateway/<gateway id lower case>", e.g. au915_1/gateway/e45f01fffe10e12e
 *   <base>/event/up      UplinkFrame
 *   <base>/event/stats   GatewayStats
 *   <base>/event/ack     DownlinkTxAck
 *   <base>/state/conn    ConnState ONLINE / OFFLINE (retained, last will)
 *   <base>/command/down  DownlinkFrame (subscribed)
 *
 * Configuration: web page (section MQTT) or console commands "mqtt" and "mqtt_cert".
 */
#ifndef _MQTT_GW_H
#define _MQTT_GW_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MQTT_GW_CERT_MAX   8192     /* max size of one PEM (bytes) */

void mqtt_gw_init(void);                       /* call once at boot */
void mqtt_gw_start(void);                      /* (re)start with the saved config; stops when disabled */
void mqtt_gw_stop(void);
bool mqtt_gw_connected(void);

/* json = Semtech PUSH_DATA JSON object ("{...}") */
void mqtt_gw_publish_uplink(const char *json, int len);

/* fills buf with a JSON status object, returns its length */
int mqtt_gw_status_json(char *buf, size_t size);

/* type: "ca", "crt" or "key"; len == 0 removes it */
esp_err_t mqtt_gw_cert_set(const char *type, const char *pem, size_t len);

void mqtt_gw_register_console(void);

/* MQTT enabled: the UDP forwarder must stay quiet (UDP and MQTT are exclusive) */
bool mqtt_gw_udp_off(void);

/* downstream thread: waits up to timeout_ms for a downlink coming from MQTT,
 * copies its Semtech {"txpk":{...}} JSON in buf and returns its length (-1 = none) */
int mqtt_gw_down_wait(char *buf, size_t size, uint32_t *id, int timeout_ms);

/* result of the downlink given by mqtt_gw_down_wait (enum jit_error_e) */
void mqtt_gw_down_result(uint32_t id, int jit_error);

#endif
