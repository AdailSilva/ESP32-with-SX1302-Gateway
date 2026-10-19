/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2023 Semtech

Description:
    Gateway version

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#ifndef __LORAGW_VERSION__
#define __LORAGW_VERSION__

/* Normally defined by the build from the Git tag (top-level CMakeLists.txt).
 * This fallback is only used when the file is compiled outside that build. */
#ifndef ESP32_SX1302_VERSION
#define ESP32_SX1302_VERSION "0.0.0-dev"
#endif

#endif
