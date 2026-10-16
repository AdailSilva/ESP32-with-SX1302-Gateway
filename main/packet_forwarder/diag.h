/*
 * Network diagnostics for the /diag web page: ping, traceroute and nslookup,
 * the same three tests of the OpenWrt "Network > Diagnostics" page.
 *
 * Each test writes its result one line at a time through out(); nothing is
 * kept in RAM after the test ends. If out() returns non-zero (browser closed
 * the connection) the test stops early.
 */
#ifndef _DIAG_H
#define _DIAG_H

#define DIAG_HOST_MAX     63   /* longest host name accepted              */
#define DIAG_PING_MAX     5    /* ping: at most 5 packets (LuCI uses 5)   */
#define DIAG_TRACE_HOPS   15   /* traceroute: at most 15 hops, 1 probe     */

typedef int (*diag_out_t)(void *ctx, const char *line);

/* true when s is a usable host name or IP literal (letters, digits, . - _ :) */
int diag_host_ok(const char *s);

void diag_ping(const char *host, int count, diag_out_t out, void *ctx);
void diag_traceroute(const char *host, diag_out_t out, void *ctx);
void diag_nslookup(const char *host, diag_out_t out, void *ctx);

#endif
