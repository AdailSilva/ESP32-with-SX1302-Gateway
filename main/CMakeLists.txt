set(libloragw_src
    "libloragw/loragw_ad5338r.c"
    "libloragw/loragw_aux.c"
    "libloragw/loragw_cal.c"
    "libloragw/loragw_com.c"
    "libloragw/loragw_debug.c"
    "libloragw/loragw_gpio.c"
    "libloragw/loragw_gps.c"
    "libloragw/loragw_hal.c"
    "libloragw/loragw_i2c.c"
    "libloragw/loragw_lbt.c"
    "libloragw/loragw_reg.c"
    "libloragw/loragw_spi.c"
    "libloragw/loragw_stts751.c"
    "libloragw/loragw_sx1250.c"
    "libloragw/loragw_sx125x.c"
    "libloragw/loragw_sx1261.c"
    "libloragw/loragw_sx1302.c"
    "libloragw/loragw_sx1302_rx.c"
    "libloragw/loragw_sx1302_timestamp.c"
    "libloragw/loragw_usb.c"
    "libloragw/sx1250_com.c"
    "libloragw/sx1250_spi.c"
    "libloragw/sx1250_usb.c"
    "libloragw/sx125x_com.c"
    "libloragw/sx125x_spi.c"
    "libloragw/sx1261_com.c"
    "libloragw/sx1261_spi.c"
    "libloragw/sx1261_usb.c"
)

set(libtools_src
    "libtools/base64.c"
    "libtools/parson.c"
    "libtools/tinymt32.c"
)

if(CONFIG_LIBLORAGW_TEST)
    set(libloragw_test_src
        "libloragw-test/test_loragw_toa.c"
        "libloragw-test/test_loragw_hal_tx.c"
        "libloragw-test/test_loragw_hal_rx.c"
        "libloragw-test/cli4test.c"
    )
    set(pkt_fwd_src "")
else()
    set(libloragw_test_src "")
    set(pkt_fwd_src
	"packet_forwarder/jitqueue.c"
	"packet_forwarder/lora_pkt_fwd.c"
        "packet_forwarder/mqtt_gw.c"
        "packet_forwarder/cs_codec.c"
        "packet_forwarder/weblog.c"
        "packet_forwarder/ioe.c"
        "packet_forwarder/led_indication.c"
        "packet_forwarder/web_config.c"
        "packet_forwarder/http_server.c"
        "packet_forwarder/diag.c"
    )
endif()


idf_component_register(SRCS
    "${libtools_src}"
    "${libloragw_src}"
    "${libloragw_test_src}"
    "${pkt_fwd_src}"
    "${main_src}"
    INCLUDE_DIRS "libloragw"
    INCLUDE_DIRS "libtools"
    INCLUDE_DIRS "libloragw-test"
    INCLUDE_DIRS "packet_forwarder"
)

# ---------------------------------------------------------------------------
# Board pin map per target (each value can still be overridden with -D...)
# ---------------------------------------------------------------------------
if(IDF_TARGET STREQUAL "esp32s3")
    # ESP32-S3 + Elecrow LR1302 LoRaWAN HAT (SPI version), wired to the HAT's RPi header.
    # Avoids GPIO 26-37 (flash/PSRAM), 19/20 (USB), 43/44 (UART0) and strapping pins 3/45/46.
    #   ESP32-S3        LR1302 HAT (RPi header pin)
    #   GPIO11  ------> MOSI   (19)
    #   GPIO13  <------ MISO   (21)
    #   GPIO12  ------> SCLK   (23)
    #   GPIO10  ------> CE0/CS (24)  SX1302 chip select
    #   GPIO6   ------> RESET  (11)  SX1302 reset, active high
    #   GPIO7   ------> CS     (31)  SX1261/SX1262 chip select, kept HIGH (deselected)
    #   GPIO4   <-----> SDA    (3)   STTS751 temperature sensor / OLED
    #   GPIO5   ------> SCL    (5)
    #   GPIO17  ------> GPS_RXD (8)  (optional)
    #   GPIO18  <------ GPS_TXD (10) (optional)
    #   5V      ------> 5V     (2/4) the HAT does not use the header 3.3V
    #   GND     ------> GND    (6)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SX1302_SPI_HOST=SPI2_HOST
        PIN_NUM_CLK=12
        PIN_NUM_MISO=13
        PIN_NUM_MOSI=11
        PIN_NUM_CS=10
        SX1302_RESET_PIN=6
        SX1302_POWER_EN_PIN=GPIO_NUM_NC     # LR1302 HAT has no POWER_EN line
        SX1261_NSS_PIN=7                    # keeps the SX1261/SX1262 off the shared SPI bus
        I2C_MASTER_SDA_IO=4
        I2C_MASTER_SCL_IO=5
        GPS_UART_TXD=17
        GPS_UART_RXD=18
        GPS_DISABLE                         # remove this line after wiring the HAT GPS (pins 8/10)
        USER_BUTTON_1=0                     # BOOT button: hold right after reset -> Soft AP mode
        USER_BUTTON_2=GPIO_NUM_NC
        BLINK_GPIO=GPIO_NUM_NC              # on-board LED of S3 boards is an RGB WS2812
        LED_BLUE_GPIO=GPIO_NUM_NC
        LED_GREEN_GPIO=GPIO_NUM_NC
        LED_RED_GPIO=GPIO_NUM_NC
    )
elseif(IDF_TARGET STREQUAL "esp32")
    # ESP32 (classic: ESP32-WROOM-32/32U DevKit) + Elecrow LR1302 LoRaWAN HAT (SPI version).
    # SPI on the native VSPI pins (18/19/23/5). GPIO12 (MTDI) is avoided because it is a
    # strapping pin: pulled high at boot it selects 1.8 V flash and the ESP32 does not boot.
    #   ESP32           LR1302 HAT (RPi header pin)
    #   GPIO23  ------> MOSI   (19)
    #   GPIO19  <------ MISO   (21)
    #   GPIO18  ------> SCLK   (23)
    #   GPIO5   ------> CE0/CS (24)  SX1302 chip select
    #   GPIO4   ------> RESET  (11)  SX1302 reset, active high
    #   GPIO25  ------> CS     (31)  SX1261/SX1262 chip select, kept HIGH (deselected)
    #   GPIO21  <-----> SDA    (3)   STTS751 temperature sensor / OLED
    #   GPIO22  ------> SCL    (5)
    #   GPIO17  ------> GPS_RXD (8)  (optional)
    #   GPIO16  <------ GPS_TXD (10) (optional)
    #   5V      ------> 5V     (2/4)
    #   GND     ------> GND    (6)
    # To build for the original ESP32_SX1302 motherboard instead, remove this elseif() block.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SX1302_SPI_HOST=SPI3_HOST
        PIN_NUM_CLK=18
        PIN_NUM_MISO=19
        PIN_NUM_MOSI=23
        PIN_NUM_CS=5
        SX1302_RESET_PIN=4
        SX1302_POWER_EN_PIN=GPIO_NUM_NC     # LR1302 HAT has no POWER_EN line
        SX1261_NSS_PIN=25
        I2C_MASTER_SDA_IO=21
        I2C_MASTER_SCL_IO=22
        GPS_UART_TXD=17
        GPS_UART_RXD=16
        GPS_DISABLE                         # remove this line after wiring the HAT GPS (pins 8/10)
        USER_BUTTON_1=0                     # BOOT button: hold right after reset -> Soft AP mode
        USER_BUTTON_2=GPIO_NUM_NC
        BLINK_GPIO=2                        # on-board blue LED of most ESP32 DevKits
        LED_BLUE_GPIO=GPIO_NUM_NC
        LED_GREEN_GPIO=GPIO_NUM_NC
        LED_RED_GPIO=GPIO_NUM_NC
    )
endif()
