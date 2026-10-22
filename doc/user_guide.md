# ESP32_SX1302 Gateway User Guide


## Basic Introduction

**ESP32_SX1302 Gateway** project is based on `SX1302_HAL` project, but uses ESP32 instead of Raspberry Pi. *ESP32_SX1302* = *ESP32* + *SX1302*.

This document is a very simple guide about how to configure and use ESP32_SX1302 gateway based on **version 0.7.0**.

First, please be noted that ESP32_SX1302 gateway has 2 types of Wi-Fi working modes, and 2 ways to configure it.

The 2 Wi-Fi modes are:
- **Soft AP** mode. The gateway works as an Wi-Fi Soft AP, so user can connect to it directly and configure it.
- **Station** mode. The gateway needs to connect to the same Wi-Fi router as the user and use that router for LoRaWAN uplink and downlink.

The 2 configuration ways are:
- **Web UI**. User can change gateway settings by browser after connect to the gateway.
- **Command line** (or Serial Port). There is a simple interactive command line interface available from the USB port, so user can configure the gateway that way without any network connection required.


## Configuration based on Wi-Fi mode

By default, the gateway boots to the Soft AP mode for the first time. It switches modes when necessary such as Wi-Fi disconnected or be controlled by user. But you can always specify which mode it boots into: just keep one of the Interface board buttons pressed during the poweron: Left button for Soft AP mode, while right button for Station mode.

In the Soft AP mode, user can connect the gateway by Wi-Fi. All the useful information can be seen from the screen, which is: `IP = "192.168.101.101"`; `Wi-Fi SSID = esp32`; `Wi-Fi password = lorawan101`.

Now user can access the web UI by browser with address: `http://192.168.101.101`. Please be noted that the protocol is **http**, not **https**. The default login information are: `username = admin`; `password = lorawan101`. It can be changed in the section "Acesso a esta página" of the web page or on the serial console with `web_auth --user <u> --pass <p>`; `web_auth --reset` restores the default. The "Sair" link at the top of the configuration and log pages logs out: the browser asks for user and password again on the next access (the user name `_logout_` is reserved for this).

If the gateway boots to Station mode, then both the gateway and user needs to connect to the same wireless router. Then the only difference is user needs to get the right IP address showing on the screen or provided by the router, and access the web UI with that IP address. It **won't** be the "192.168.101.101" any more.

There are also some Wi-Fi mode automatic switch if some events happens:
- In Soft AP mode, if no changes to the web UI and no input to the command line, it reboots to Station mode in 10 minutes.
- If gateway boots to Station mode, but can't connect to wireless router (maybe wrong Wi-Fi settings or haven't set Wi-Fi yet), then it reboots to Soft AP mode in 5 minutes.
- If during the Station mode, `pkt_fwd` works all right for some time, then Wi-Fi disconnected, the gateway will try to re-connect to Wi-Fi for 5 minutes. If succeeds during that time, nothing happens; but if failed, then reboots to Soft AP mode.
- If after power on, the gateway reboots again by `brownout`, then it changes to the other mode. So user should check the screen or the command line output to be sure which mode the gateway is really in.


## Configuration by web UI

From web UI, user can configure below items:
- **next time boot into**. Generally after the configuration is done, the gateway should boot into station mode. But if the gateway will boot somewhere with some unknown Wi-Fi router and needs to provide the SSID informatioin again, then boots into Soft AP mode maybe make more sense. Consider you can always control which mode boots into by the hardware button, this option only becomes important when you can't access the gateway hardware easily.
- **Frequency Plan**. You need to choose this wisely based on your region and node settings. You can click each of them to know the details of the original JSON file used for that frequency plan.
- **AU915 (Brazil / Australia)**. Two plans are available, each one covering one AU915-928 sub-band (8 x 125 kHz + 1 x 500 kHz), since the SX1302 listens to 8+1 channels at once. **AU915 SB1** is the default when no plan has been saved yet. Set the end-devices / network server (e.g. ChirpStack) to the same sub-band.

  | Plan | 125 kHz uplink channels | 500 kHz channel | Radio 1 (radio_0) | Radio 2 (radio_1) |
  |------|-------------------------|-----------------|-------------------|-------------------|
  | AU915 SB0 | ch 0-7: 915.2 / 915.4 / 915.6 / 915.8 / 916.0 / 916.2 / 916.4 / 916.6 MHz | ch 64: 915.9 MHz (SF8BW500) | 915600000 | 916300000 |
  | AU915 SB1 | ch 8-15: 916.8 / 917.0 / 917.2 / 917.4 / 917.6 / 917.8 / 918.0 / 918.2 MHz | ch 65: 917.5 MHz (SF8BW500) | 917200000 | 917900000 |

  Radio 1 channels use IF offsets -400K/-200K/0K/+200K Hz (plus +300K Hz for the 500 kHz channel); Radio 2 channels use -300K/-100K/+100K/+300K Hz. Downlinks (RX1/RX2) are sent by radio_0 between 923.3 and 927.5 MHz (500 kHz BW), within the allowed 915-928 MHz TX range. With an AU915 plan selected, saved Radio center frequencies outside 915-928 MHz are ignored, so it's safe to leave the fields empty. **Hardware note**: the SX1302/SX1250 board and its RF matching/SAW filter must be the 915 MHz version (the reference design in `hardware_design/` is the CN470 one).
- **Radio 1 and Radio 2 center frequency**. These 2 values should be the reasonable values based on the Frequency plan. Each control the frequency bands. The frequency bands has offsets of -300K/-100K/100K/300K Hz with the center frequency for CN470, while it's -400K/-200K/0K/200K for EU868. So please check the Frequency plan link to be sure.
- **Wi-Fi SSID and password**. Please noted that both only support the ASCII characters. It doesn't work if your SSID or password contains other UTF-8 characters such as Chinese characters. This limitation is hard to remove so you may have to change your Wi-Fi router settings.
- **NS Host and Port**. Here user needs to provide the right NS IP address (or domain name) and port.
- **Gateway ID**. Each gateway needs to have its own unique ID, so provide it here to avoid using the same one as other gateway.

If you think the original value is correct for any of above fields, then you can just leave it empty. Any value you provided would be used to replace the original one from the JSON template file.

You can save your configurations whenever you like by clicking the **Apply** button. Such changes are saved in the gateway but won't apply to the current *packet-forward* process yet. It only happens after you click the **Reboot** button to reboot the gateway, or by user reboot the gateway by power off and on.

If you don't like the default dark theme, you can try the light theme by accessing `http://<gateway_ip>/w` instead.


## Configuration by command line

If you power on the gateway by connecting the micro USB cable to a PC, then you have access to its command line interface. Use any terminal tool such as `sscom` or `putty` to access it with configuration as: **115200-8-N-1**.

The command line is the same thing as the serial port, so it outputs gateway logs. When the *packet_forward* is running, the output would mix with any characters input by user. However, if the user ignores that and continues to input or delete characters, the command line will flush itself, so it is still usable. 

When the *packet_forward* doesn't run for reasons such as when Wi-Fi is not connected, then the command line looks like a regular Linux shell interactive window; it even supports basic readline shortcuts such as `Ctrl-A` and `Ctrl-W` etc.

So far, this interface only supports one command named `pkt_fwd`. You can input `pkt_fwd -h` to learn all the options it supports, which, let's be honest, not much. It supports only parts of all the configurations available from web UI. Here is the output of `pkt_fwd -h`:
```
 ---- pkt_fwd ----

Available options:
 -h                   print this help
 -u <wifi ssid>       Wifi SSID
 -p <wifi password>   Wifi Password
 --host <NS Host>     NS Host
 --port <NS Port>     NS Port
 --gwid <gateway id>  Gateway ID
```

So, if you need to change other configuration items, please use the web UI way.

**Note:**
- Whenever you provide a valid option to `pkt_fwd`, it's saved and the gateway will reboot to apply the new changes.
- To input whitespace in command line (such as for Wi-Fi password, use `\` to escape. For example: `pkt_fwd -u mywifi -p my\ password`.

### install Windows Driver

To access the command line under Windows, users would need to install the right device driver first.

Below is the list of USB to serial converter chips installed on most of the ESP32 boards produced by Espressif together with links to the drivers:

- CP210x: [CP210x USB to UART Bridge VCP Drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- FTDI: [FTDI Virtual COM Port Drivers](https://ftdichip.com/drivers/vcp-drivers)

User can blindly install both drivers, or identifies the USB-to-UART bridge on the ESP32 board first and chooses the right one.

One way to identify the bridge under Linux is using the `lsusb` command. Run it would give output similar as **Bus 002 Device 027: ID 10c4:ea60 Silicon Labs `CP210x` UART Bridge** or **Bus 002 Device 025: ID 1a86:7523 QinHeng Electronics `CH340` serial converter**.


## Other Useful Documents

For other useful information, please refer to other documents included in the project, include the `README.md`, and a few others located in the folder `doc` including `esp32_sx1302_upgrade_notes.md`, `notes_tips.md` and `todo_issue_list.md`.


## ESP32-S3

The firmware also builds for the ESP32-S3 (the pin map lives in `main/CMakeLists.txt` and can be changed there):

```bash
idf.py set-target esp32s3        # once; uses sdkconfig.defaults + sdkconfig.defaults.esp32s3
./run_me.sh make                 # (re)generates webpage.h / global_json.h and builds the app
idf.py -p /dev/ttyACM0 flash monitor
```

Default wiring: ESP32-S3 + Elecrow LR1302 LoRaWAN HAT (SPI version), using the HAT's Raspberry Pi header.

| Function | ESP32-S3 GPIO | LR1302 HAT header pin | Notes |
|----------|---------------|-----------------------|-------|
| SX1302 MOSI | 11 | 19 | SPI2 (FSPI) |
| SX1302 MISO | 13 | 21 | |
| SX1302 SCK  | 12 | 23 | |
| SX1302 CS (CE0) | 10 | 24 | |
| SX1302 RESET | 6 | 11 | active high |
| SX1261/SX1262 CS | 7 | 31 | held HIGH so it never drives MISO |
| I2C SDA (STTS751 / OLED) | 4 | 3 | |
| I2C SCL (STTS751 / OLED) | 5 | 5 | |
| ESP TX -> GPS RX | 17 | 8 | optional |
| GPS TX -> ESP RX | 18 | 10 | optional |
| 5V | 5V | 2 / 4 | the HAT does not use the header 3.3V |
| GND | GND | 6 | |
| Button 1 (Soft AP) | 0 | - | on-board BOOT button, hold right after reset |

The LR1302 HAT has no POWER_EN line (`SX1302_POWER_EN_PIN=GPIO_NUM_NC`). LEDs and button 2 are disabled by default.
GPIO 26-37 (flash/PSRAM), 19/20 (USB), 43/44 (UART0) and 3/45/46 (strapping) are intentionally not used.
On the S3 the console (logs and the `ESP32_SX1302_GW>` command line) runs on the chip's native USB port (`/dev/ttyACM0`).


## ESP32 (classic) + LR1302 HAT

For an ESP32-WROOM-32/32U DevKit the pin map is in the `elseif(IDF_TARGET STREQUAL "esp32")` block of `main/CMakeLists.txt`
(remove that block to go back to the original ESP32_SX1302 motherboard pins).

```bash
rm -rf build sdkconfig            # leaving a previous ESP32-S3 build
idf.py set-target esp32
./run_me.sh make
idf.py -p /dev/ttyUSB0 flash monitor
```

| Function | ESP32 GPIO | LR1302 HAT header pin | Notes |
|----------|------------|-----------------------|-------|
| SX1302 MOSI | 23 | 19 | VSPI (SPI3_HOST) |
| SX1302 MISO | 19 | 21 | |
| SX1302 SCK  | 18 | 23 | |
| SX1302 CS (CE0) | 5 | 24 | |
| SX1302 RESET | 4 | 11 | active high |
| SX1261/SX1262 CS | 25 | 31 | held HIGH |
| I2C SDA | 21 | 3 | |
| I2C SCL | 22 | 5 | |
| ESP TX -> GPS RX | 17 | 8 | optional |
| GPS TX -> ESP RX | 16 | 10 | optional |
| 5V / GND | 5V / GND | 2 / 6 | |
| Button 1 (Soft AP) | 0 | - | on-board BOOT button |
| Status LED | 2 | - | on-board LED |

GPIO12 is not used: it is a strapping pin (flash voltage) and must stay low while the ESP32 boots.


## Settings: web page or console

The web page and the console write the same settings (NVS), so a setting made in either place is applied the same
way at boot. MQTT, the TLS certificates and the web login are applied at once; the others after a reboot
("Reiniciar agora" on the page, or `reboot` / `config --reboot` on the console).

```
config                                          # show every saved setting
config --region au915_sb1                       # au915_sb0 | au915_sb1 | us915 | eu868 | cn470
config --radio0 917200000 --radio1 default      # radio center frequencies (default = plan default)
config --ntp br.pool.ntp.org --mode station
config --ssid MyWiFi --wifi-pass secret123 --host ns.example.com --port 1700 --gwid E45F01FFFE10E12E
config --reboot                                 # reboot to apply
reboot
```

## MQTT over TLS

Besides the Semtech UDP forwarding, the gateway can publish to an MQTT broker, optionally over TLS
(`main/packet_forwarder/mqtt_gw.c`). Configure it in the web page (section "MQTT seguro (TLS)") or on the serial console.

**UDP and MQTT are exclusive**: with MQTT enabled the gateway sends only by MQTT and the UDP forwarder is off;
with MQTT disabled it sends only by UDP (the host / port of "Servidor LoRaWAN" are kept for that).

Two formats ("Formato" in the web page, `mqtt --format` on the console):

### ChirpStack v4 (`chirpstack`)

The gateway speaks the ChirpStack v4 gateway protocol directly (the messages the ChirpStack Gateway Bridge
publishes, in JSON). No Gateway Bridge and no UDP port are needed.
Base topic default: `<region>/gateway/<gateway id in lower case>`, e.g. `au915_1/gateway/e45f01fffe10e12e`
(the region prefix must be the one of the ChirpStack region: AU915 SB0 = `au915_0`, SB1 = `au915_1`).

| Topic | Message |
|-------|---------|
| `<base>/event/up` | UplinkFrame (one per received packet) |
| `<base>/event/stats` | GatewayStats (every stats interval, 30 s) - this is what makes the gateway "online" |
| `<base>/event/ack` | DownlinkTxAck |
| `<base>/state/conn` | ConnState ONLINE / OFFLINE (retained, last will) |
| `<base>/command/down` | DownlinkFrame (subscribed): the alternatives (RX1, RX2) are tried in order |

Encoding ("Codificação" in the web page, `mqtt --encoding json|protobuf` on the console): **JSON** (default,
readable in any MQTT client) or **Protobuf** (binary, smaller, the same as the Gateway Bridge). ChirpStack v4 accepts
both: it detects JSON when the payload contains `gatewayId`, and answers each gateway in the encoding of its last
event. The gateway reads the downlinks in either encoding. The MQTT user of the gateway needs publish rights on `<base>/event/#` and `<base>/state/#` and subscribe
rights on `<base>/command/#`. ChirpStack v3 is not supported by this mode (different JSON); use the UDP forwarder.

### Semtech mirror (`semtech`, default)

Semtech JSON of the UDP protocol, uplinks and statistics only (no downlinks). Topics (base topic default `gateway/<Gateway ID>`):

| Topic | Payload |
|-------|---------|
| `<base>/up` | `{"gw":"<id>","rxpk":[...]}` (Semtech JSON of the received packets), QoS 1 |
| `<base>/stats` | `{"gw":"<id>","stat":{...}}` every stats interval, QoS 1 |
| `<base>/state` | `online` / `offline` (retained, last will) |

TLS: with `mqtts://` and no CA installed, the broker certificate is checked against the ESP-IDF public CA bundle
(Let's Encrypt etc.). Install the broker CA for a private PKI; install client certificate + key for mutual TLS.
Certificates are stored in NVS (namespace `mqtt_tls`), max 8 KB each.

Console:

```
mqtt                                              # status
mqtt --on --uri mqtts://broker.example.com:8883 --user gw01 --pass secret
mqtt --topic lorawan/gw01      mqtt --clear-topic  mqtt --off
mqtt --format chirpstack --topic au915_1/gateway/e45f01fffe10e12e
mqtt --encoding protobuf        mqtt --encoding json
mqtt --format semtech
mqtt_cert ca                                      # then paste the PEM
mqtt_cert crt / mqtt_cert key                     # client certificate / key (mutual TLS)
mqtt_cert ca clear                                # remove
```

Changes made on the console, by "Salvar" on the web page or by uploading a certificate are applied immediately.


## Logs in the web page

Open `http://<gateway IP>/logs` (or the "Logs" link at the top of the configuration page). It shows the same output
as the serial console, updated every 1.5 s, with filter, pause and auto-scroll. The last 16 KB are kept in RAM
(`WEBLOG_SIZE` in `main/packet_forwarder/weblog.h`); older lines are discarded. Two download buttons: "Baixar do gateway" (`/logs_download`, everything still in the gateway RAM) and
"Baixar sessão" (everything the page received since it was opened, up to 3000 lines). Messages printed before the
first boot step (bootloader, `cpu_start`) only appear on the serial port.

