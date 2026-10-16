# CHANGE LOG


## v0.0.1 (first release of ESP32-with-SX1302-Gateway):
- two supported boards, one source tree: ESP32-WROOM-32U DevKit (IDF_TARGET=esp32, current feature set)
  and ESP32-S3-WROOM-2 (N32R16V) DevKit (IDF_TARGET=esp32s3, current feature set + new features)
- sdkconfig.defaults.esp32s3: 32 MB octal flash + 16 MB octal PSRAM (the WROOM-2 N32R16V does not boot without them)
- scripts/gen_headers.sh (webpage.h / global_json.h) and scripts/build.sh <esp32|esp32s3> [flash <port>]:
  one build directory and one sdkconfig per target (build-<target>/, sdkconfig.<target>)
- GitHub Actions: builds both targets on every push/PR, publishes a Release with the binaries on v* tags
- .gitignore: sdkconfig.defaults* were ignored by "sdkconfig.*"; build-*/, managed_components/, logs/ ignored
- platformio.ini: environments esp32dev and esp32s3 only (esp32s3 with opi flash, 32 MB)
- README in pt-BR with colored pinout / wiring / architecture diagrams (doc/images/*.svg) and
  per-board wiring guides (doc/hardware/)
- version numbering restarts at 0.0.1 for this repository (ESP32_SX1302_VERSION "0.0.1")


---

# History of the original ESP32_SX1302 project (before this repository)

The versions below come from the original port and its AU915 adaptation. They are kept for reference;
the tags of this repository start at v0.0.1.

## v0.9.1 (AU915):
- web status panel ("Status" section at the top of the configuration page): uptime, free / minimum free heap,
  Wi-Fi RSSI, concentrator temperature, packets received (CRC ok / error / no CRC), forwarded, downlinks
  (requested / sent / failed / rejected) and age of the last server ACK with PUSH/PULL ack ratio; counters are
  totals since boot (kept apart from the per-report counters, ~70 bytes of RAM)
- GET /status returns {"gw":{...},"mqtt":{...}} and replaces /mqtt_status: the page makes one request every 5 s
- web polling chained with setTimeout (no overlapping requests when the gateway is slow) and paused while the
  browser tab is hidden, on the configuration page and on /logs
- fix: side menu marked "Acesso" after clicking "Inicialização" (the last sections can not reach the top of
  the screen); the clicked item now stays marked until the page is scrolled by hand
- fix: with MQTTS enabled no message was sent (0 sent): the TLS session started in parallel with the
  concentrator init and took the RAM of the thread_up stack ("Failed to spawn thread_up", largest free
  block 13 KB). MQTT now starts after thread_up/down/jit, a failed thread spawn reboots instead of failing
  silently, the free heap is logged at each stats cycle, and mbedTLS uses dynamic buffers (sdkconfig.defaults)
- add AU915-928 frequency plans (Brazil / Australia): "AU915 SB0" (ch 0-7 + 64) and "AU915 SB1" (ch 8-15 + 65)
- AU915 SB1 becomes the default plan when none has been saved
- ignore saved radio center frequencies outside 915-928 MHz when an AU915 plan is selected
- Class B beacon parameters for AU915 (923.3 MHz + n*600 kHz, SF12BW500)
- web UI: AU915 options, fix Radio 2 frequency validation, increase max URI handlers, enlarge JSON view buffer
- regenerate main/global_json.h from the JSON files (the embedded copy was out of date)
- default NTP server set to br.pool.ntp.org
- build fixes for ESP-IDF 6.x / GCC 16: speed_t -> uint32_t in lgw_gps_enable(), FreeRTOS include in loragw_spi.c,
  4-byte buffers initialized with {0} instead of string literals, WIFI_IF_STA instead of ESP_IF_WIFI_STA,
  esp-mqtt fetched from the Component Registry (main/idf_component.yml) since it left the IDF core in v6.0
- ESP32-S3 support: pin map in main/CMakeLists.txt, console/REPL on the native USB Serial/JTAG port,
  GPIO_NUM_NC handled for reset/power, buttons and blink LED, platformio env "esp32s3"
- default options moved from "sdkconfig" to "sdkconfig.defaults" (+ "sdkconfig.defaults.esp32s3"),
  so "idf.py set-target" works
- ESP32-S3 pin map matches the Elecrow LR1302 HAT wiring (RESET=6, SDA=4, SCL=5, SX1261 CS=7 held high, no POWER_EN)
- optional SX1261_NSS_PIN, driven high before the SX1302 reset
- on concentrator start failure, print a wiring hint and wait 20 s before rebooting
- ESP32 (classic) + LR1302 HAT pin map: VSPI 18/19/23/5, RESET=4, SX1261 CS=25, I2C 21/22, GPS 17/16
- web login default admin / lorawan, changeable in the web page and with the console command "web_auth" (--reset restores the default)
- web pages: "Desenvolvido por AdailSilva" credit in the footer, linked to https://k8s.adailsilva.com.br/
- console "config" (all the settings of the web page) and "reboot"; web and console share the same NVS settings
- web: radio frequencies can be cleared (back to the plan default); passwords masked in the form log
- UDP and MQTT are exclusive: MQTT enabled = UDP forwarder off (any MQTT format)
- fix: web form field "mqtt_enc" was read as "mqtt_en" (prefix match), so MQTT could not be enabled from the page
- web page icon (favicon: antenna tower with radio waves), served at /favicon.svg and /favicon.ico
- MQTT format "ChirpStack v4" in JSON or Protobuf (hand written encoder/decoder, checked against the official .proto): UplinkFrame / GatewayStats / DownlinkTxAck / ConnState in JSON, downlinks from command/down (RX1/RX2 alternatives), UDP off in this mode (cs_codec.c); console "mqtt --format", web option
- web logout: "Sair" link on the configuration and log pages (/logout replaces the browser saved login, /saiu page)
- log download: "Baixar do gateway" (/logs_download, whole RAM buffer, colors removed) and "Baixar sessão" (what the page received)
- web log viewer: page /logs (live, filter, pause), stdout tee into a 16 KB RAM ring buffer (weblog.c), link in the header
- project renamed ESXP1302 -> ESP32_SX1302 (folder esp32_sx1302/, binary ESP32_SX1302-Pkt-Fwd.bin, ESP32_SX1302_VERSION)
- MQTT/MQTTS client (mqtt_gw.c): uplinks/stats/state topics, TLS with custom CA or public CA bundle,
  mutual TLS (client cert + key), user/password; managed from the web page (status, certificate upload)
  and console commands "mqtt" / "mqtt_cert"; replaces the old unused MQTT stub
- web page sent in chunks with HTML-escaped values (no 12 KB RAM buffer)
- gateway name "ESP32 with SX1302 Gateway" (banner, OLED, console prompt, web pages), set in packet_forwarder/branding.h
- new web configuration page (pt-BR, light/dark, phone friendly, inline validation, hyphen allowed in host, password show/hide)
- form POST read completely (up to 1 KB, was 256 bytes); new "saved" and "rebooting" pages
- scripts/dump_html.py embeds the page as UTF-8 bytes
- GPS disabled by default on the LR1302 HAT pin maps (GPS_DISABLE; remove it after wiring the GPS)
- SX1302 wiring diagnostic on start failure (MISO level with CS high/low, both reset polarities, 2 MHz / 500 kHz)
- LEDs set to GPIO_NUM_NC are skipped at init; MISO weak pull-up + chip version diagnostics (0x00 / 0xFF)

## v0.9.0:
- upgrade to sx1302_hal v2.1.0
- support nodo board
- add option to set NTP server address
- extends us915 TX frequency to cover the range of AS923
- allow up to 1.5MB app size
- remove black web theme
- other fixes to make esp32_sx1302 be more flexible for different hardware designs.


## v0.8.0:
- add hardware reference design materials
- add support for esp32c3
- add support for compiling using PlatformIO
- add support for compiling under Windows (based on PlatformIO)
- optimize memory usage
- update scripts to Python3
- other fixes and document improvement


## v0.7.0:
- show version info in the screen
- change CN470 tx_freq_min from 500MHz to 470MHz for supporting RP2
- fix the bug that ns host can't be domain name
- change web username/password as 'iot/lora'
- add upgrader.py to simplify upgrade the firmware
- add license info


## v0.6.2:
- disable gps output
- workaround to support sx1303


## v0.6.1:
- hide wifi password


## v0.6.0:
- the first public released version
- add version info in the serial port output
- documents
