/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2019 Semtech

Description:
    Functions to reset LoRa concentrator from GPIO Pins.

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#ifndef _LORAGW_GPIO_H
#define _LORAGW_GPIO_H


#include "driver/gpio.h"

#ifndef SX1302_RESET_PIN
#define SX1302_RESET_PIN          2
#endif

#ifndef SX1302_POWER_EN_PIN
#define SX1302_POWER_EN_PIN       4
#endif

#define SX1302_PIN_BIT(p)         (((int)(p) >= 0) ? (1ULL << (p)) : 0ULL)
#define SX1302_GPIO_PIN_SEL       (SX1302_PIN_BIT(SX1302_RESET_PIN) | SX1302_PIN_BIT(SX1302_POWER_EN_PIN))

/* optional: chip select of the SX1261/SX1262 that shares the SPI bus on some boards
 * (e.g. LR1302 HAT). It is driven HIGH so that chip never answers on MISO. */
#ifndef SX1261_NSS_PIN
#define SX1261_NSS_PIN            GPIO_NUM_NC
#endif

// reset the gateway using RESET and POWER_EN GPIO Pins.
void lgw_reset(void);

#endif
