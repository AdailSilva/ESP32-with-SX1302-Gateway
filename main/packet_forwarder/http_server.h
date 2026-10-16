/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2023 Semtech

Description:
    Http service head file

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#ifndef _HTTP_SERVER_H
#define _HTTP_SERVER_H

#include <stddef.h>


/* default login of the web page (can be changed in the page or with the console command "web_auth") */
#define BASIC_AUTH_USERNAME "admin"
#define BASIC_AUTH_PASSWORD "lorawan"

/* fake login used only by the "Sair" (logout) button; the web user can not be this one */
#define WEB_LOGOUT_USER "_logout_"
#define WEB_LOGOUT_PASS "_logout_"
#define WEB_AUTH_MAX_LEN    31

/* loads the saved login (or the defaults) into the running web server */
void web_auth_apply(void);
/* console command "web_auth" */
void web_auth_register_console(void);

#define HTTPD_401      "401 UNAUTHORIZED"           /*!< HTTP Response 401 */

void http_server_task(void *pvParameters);

/* status of the gateway as JSON (uptime, heap, Wi-Fi, packet counters since boot),
 * implemented in lora_pkt_fwd.c; returns what snprintf returns */
int pkt_fwd_status_json(char *buf, size_t size);

#endif
