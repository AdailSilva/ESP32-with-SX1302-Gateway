# Ligações: ESP32-S3-WROOM-2 (N32R16V) DevKit ↔ Elecrow LR1302 LoRaWAN HAT (SPI)

Alvo **`esp32s3`**. É a pinagem compilada pelo bloco `if(IDF_TARGET STREQUAL "esp32s3")` de [`main/CMakeLists.txt`](../../main/CMakeLists.txt).

> **Referência dos lados:**
> - **ESP32-S3:** layout da ESP32-S3-DevKitC-1 (2 × 22 pinos), vista de cima, com os conectores USB para baixo. Placas genéricas podem ter outra ordem física, mas **o que vale é o número do GPIO** impresso na placa.
> - **HAT:** o header de 40 pinos segue o padrão do Raspberry Pi. O pino 1 é o quadrado, marcado "1"/"3V3".
>
> **Por que esses GPIOs** (módulo **WROOM-2 N32R16V**, com flash e PSRAM octais em **1,8 V**):
> - **GPIO26–32:** flash.
> - **GPIO33–37:** PSRAM octal.
> - **GPIO47/48:** ficam no domínio de 1,8 V; não ligar sinais de 3,3 V.
> - **GPIO19/20:** USB nativo, onde fica o console.
> - **GPIO43/44:** UART0.
> - **GPIO0/3/45/46:** strapping.
>
> A pinagem abaixo evita todos eles.

<p align="center"><img src="../images/pinout_esp32s3_wroom2_n32r16v_devkit.svg" alt="Pinagem da ESP32-S3-WROOM-2 N32R16V DevKit" width="100%"/></p>
<p align="center"><img src="../images/wiring_esp32s3_wroom2.svg" alt="Ligação com o LR1302 HAT" width="100%"/></p>

---

## 1. Tabela de ligações

| # | Sinal | ESP32-S3 (GPIO) | Sentido | HAT (pino físico) | Nome no padrão RPi |
|---|-------|-----------------|:-------:|-------------------|--------------------|
| 1 | SPI MOSI | **GPIO11** | → | **19** | GPIO10 / MOSI |
| 2 | SPI MISO | **GPIO13** | ← | **21** | GPIO9 / MISO |
| 3 | SPI SCLK | **GPIO12** | → | **23** | GPIO11 / SCLK |
| 4 | CS do SX1302 | **GPIO10** | → | **24** | GPIO8 / CE0 |
| 5 | RESET do SX1302 (ativo em nível alto) | **GPIO6** | → | **11** | GPIO17 |
| 6 | CS do SX1261 (fica sempre em nível alto) | **GPIO7** | → | **31** | GPIO6 |
| 7 | I2C SDA (sensor de temperatura / OLED) | **GPIO4** | ↔ | **3** | GPIO2 / SDA |
| 8 | I2C SCL | **GPIO5** | → | **5** | GPIO3 / SCL |
| 9 | GPS RX do HAT *(opcional)* | **GPIO17** (TX) | → | **8** | GPIO14 / TXD |
| 10 | GPS TX do HAT *(opcional)* | **GPIO18** (RX) | ← | **10** | GPIO15 / RXD |
| 11 | Alimentação 5 V | **5V** | → | **2** e/ou **4** | 5V |
| 12 | Terra | **GND** | — | **6** (e 9, 14, 20, 25…) | GND |

**SPI:** GPIO10–13 são os pinos nativos (IOMUX) do FSPI/SPI2 do S3 (`SX1302_SPI_HOST=SPI2_HOST`). Com eles o sinal não passa pela matriz de GPIO e aguenta clock mais alto.

**Não ligar:**
- **3V3 do header (pinos 1 e 17).** O HAT gera os próprios 3,3 V a partir dos 5 V.
- **POWER_EN.** O LR1302 HAT não tem essa linha (`SX1302_POWER_EN_PIN=GPIO_NUM_NC`).

**Já na placa, sem fio extra:**
- **GPIO0:** botão BOOT. Se estiver pressionado logo após o reset, o gateway entra em modo Soft AP.
- **LED:** o LED da DevKit é RGB (WS2812), por isso `BLINK_GPIO=GPIO_NUM_NC`. Em algumas DevKits ele fica no GPIO48, que neste módulo está no domínio de 1,8 V.

**Livres para expansão** (LED de status, botão, PPS do GPS etc.): GPIO1, 2, 8, 9, 14, 15, 16, 21, 38–42.

---

## 2. Pinout da ESP32-S3-DevKitC-1 (2 × 22) em texto

```
                        ┌─────────────────────────┐
                        │  ESP32-S3-WROOM-2       │
                        │  N32R16V (antena PCB)   │
                   ┌────┴─────────────────────────┴────┐
            3V3 ───┤ 1                               1 ├─── GND ◄────────── GND (HAT 6)
            3V3 ───┤ 2                               2 ├─── GPIO43 (TX0)
            RST ───┤ 3                               3 ├─── GPIO44 (RX0)
SDA  (HAT 3) ►GPIO4┤ 4                               4 ├─── GPIO1
SCL  (HAT 5) ►GPIO5┤ 5                               5 ├─── GPIO2
RESET(HAT11) ►GPIO6┤ 6                               6 ├─── GPIO42
CS1261(HAT31)►GPIO7┤ 7                               7 ├─── GPIO41
         GPIO15 ───┤ 8                               8 ├─── GPIO40
         GPIO16 ───┤ 9                               9 ├─── GPIO39
GPS RXD(HAT 8)►G17 ┤ 10                             10 ├─── GPIO38
GPS TXD(HAT10)►G18 ┤ 11                             11 ├─── GPIO37 (PSRAM) ✗
          GPIO8 ───┤ 12                             12 ├─── GPIO36 (PSRAM) ✗
        ✗ GPIO3 ───┤ 13  (strapping)                13 ├─── GPIO35 (PSRAM) ✗
       ✗ GPIO46 ───┤ 14  (strapping)                14 ├─── GPIO0  (botão BOOT → Soft AP)
          GPIO9 ───┤ 15                             15 ├─── GPIO45 (strapping) ✗
CS SX1302(24)►G10  ┤ 16                             16 ├─── GPIO48 (1,8 V!) ✗
MOSI (HAT19) ►G11  ┤ 17                             17 ├─── GPIO47 (1,8 V!) ✗
SCLK (HAT23) ►G12  ┤ 18                             18 ├─── GPIO21
MISO (HAT21) ►G13  ┤ 19                             19 ├─── GPIO20 (USB D+) ✗
         GPIO14 ───┤ 20                             20 ├─── GPIO19 (USB D−) ✗
  5V ◄─────────────┤ 21                             21 ├─── GND
  └─► HAT 2/4  GND ┤ 22                             22 ├─── GND
                   └──────[ USB-UART ]────[ USB nativo ]──┘
                                          (console /dev/ttyACM0)

  ► = pino usado pelo gateway       ✗ = não usar
```

---

## 3. Header do HAT

<p align="center"><img src="../images/lr1302_hat_header.svg" alt="Header de 40 pinos do LR1302 HAT" width="100%"/></p>

---

## 4. Alimentação

<p align="center"><img src="../images/power.svg" alt="Alimentação" width="100%"/></p>

- A transmissão LoRa gera **picos de corrente**. Em uso contínuo, alimente pelos **5 V de uma fonte de 2 A**, não só pelo USB.
- **O GND precisa ser comum** a tudo.
- A lógica do HAT é **3,3 V**. Todos os GPIOs usados (4–7, 10–13, 17, 18) são de 3,3 V no S3, então não precisa de conversor de nível.

---

## 5. Configuração do firmware para este módulo

O [`sdkconfig.defaults.esp32s3`](../../sdkconfig.defaults.esp32s3) já traz as opções do **WROOM-2 N32R16V**. Sem elas o módulo não inicia, porque a flash e a PSRAM são **octais**:

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESPTOOLPY_OCT_FLASH=y
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
```

Mover a sessão TLS e as filas para a PSRAM fica para a próxima etapa (veja o roadmap no README).

```bash
./scripts/build.sh esp32s3 flash /dev/ttyACM0     # porta "USB" (nativa), não a "UART"
```

---

## 6. Observações

1. **Fios SPI curtos**, idealmente com menos de 10–15 cm e de mesmo comprimento.
2. **CS do SX1261 (GPIO7 → pino 31):** o firmware mantém esse pino em nível **alto** antes do reset do SX1302, para o SX1261 não disputar o barramento SPI. Não deixe esse fio solto.
3. **RESET (GPIO6 → pino 11)** é ativo em **nível alto**. O firmware gera o pulso sozinho no boot.
4. **Sensor de temperatura:** o firmware procura um STTS751 em 0x39, 0x3B e 0x38. Se não encontrar, usa 25 °C na compensação de RSSI.
5. **GPS:** remova `GPS_DISABLE` em `main/CMakeLists.txt` depois de ligar os pinos 8/10. O PPS ainda não está mapeado no firmware. Se for usar, ligue o PPS do HAT a um GPIO livre, por exemplo GPIO16 ou GPIO21.
6. **Antena Wi-Fi:** o WROOM-2 só tem antena de PCB. Deixe a ponta do módulo, onde fica a antena, para fora da caixa e longe do HAT e de partes metálicas.
7. **Antena LoRa:** nunca ligue o gateway sem antena no conector LoRa do HAT.
