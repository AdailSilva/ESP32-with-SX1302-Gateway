/*
 * Gateway name shown on the boot banner, OLED display, console prompt and web pages.
 * Change it here (and in packet_forwarder/webpage.html: <title> and <h1>).
 */
#ifndef _GW_BRANDING_H
#define _GW_BRANDING_H

#define GW_NAME         "ESP32 with SX1302 Gateway"  /* boot banner, web pages   */
#define GW_NAME_SHORT   "ESP32 SX1302"         /* short form, page titles   */
#define GW_NAME_OLED    "ESP32+SX1302 GW"      /* OLED title, max 16 chars  */
#define GW_PROMPT       "ESP32_SX1302>"        /* serial console prompt     */

#endif
