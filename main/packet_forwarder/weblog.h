/*
 * Web log: copies everything written to stdout (printf, ESP_LOGx) into a RAM
 * ring buffer, while still sending it to the serial console. The web page
 * /logs reads it through /logs_data?from=<offset>.
 */
#ifndef _WEBLOG_H
#define _WEBLOG_H

#include <stdint.h>
#include <stddef.h>

#define WEBLOG_SIZE   16384     /* bytes kept in RAM */

void weblog_init(void);         /* call as early as possible in app_main() */

/* copies up to max bytes starting at absolute offset 'from'.
 * *start = oldest offset still available, *next = offset for the next call */
size_t weblog_read(uint32_t from, char *dst, size_t max, uint32_t *start, uint32_t *next);

#endif
