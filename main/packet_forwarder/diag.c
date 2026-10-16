/*
 * Network diagnostics for the /diag web page (ping, traceroute, nslookup).
 *
 * Memory: nothing is allocated while idle. Ping and traceroute share one raw
 * ICMP socket that exists only while the test runs (the same socket type
 * esp_ping uses, but without its extra task). The buffers live on the stack
 * of the HTTP server task (~400 bytes). The HTTP server runs one request at a
 * time, so two tests never run together.
 *
 * Ping and traceroute are IPv4 only; nslookup shows IPv4 and IPv6 addresses.
 * The output follows the BusyBox tools used by OpenWrt (ping -c N -W 1,
 * traceroute -q 1 -w 1 -n, nslookup), so it reads the same way.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/icmp.h"

#include "diag.h"

#define DIAG_LINE        128
#define DIAG_DATA        32      /* ICMP payload bytes (BusyBox default is 56) */
#define DIAG_WAIT_US     1000000 /* 1 s per probe, like -W 1 / -w 1          */

typedef struct {
    diag_out_t out;
    void *ctx;
    bool stop;                   /* browser went away */
} diag_io_t;

static void say(diag_io_t *io, const char *fmt, ...)
{
    char line[DIAG_LINE];
    va_list ap;

    if (io->stop)
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (io->out(io->ctx, line))
        io->stop = true;
}

int diag_host_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;

    if (n == 0 || n > DIAG_HOST_MAX)
        return 0;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '.' || c == '-' || c == '_' || c == ':'))
            return 0;
    }
    return 1;
}

static int resolve4(diag_io_t *io, const char *tool, const char *host, struct sockaddr_in *to, char *ip)
{
    struct addrinfo hints = { .ai_family = AF_INET }, *res = NULL;

    if (strchr(host, ':')) {
        say(io, "%s: só IPv4 é suportado (\"%s\" é IPv6)\n", tool, host);
        return -1;
    }
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        say(io, "%s: não foi possível resolver \"%s\"\n", tool, host);
        return -1;
    }
    memcpy(to, res->ai_addr, sizeof(*to));
    freeaddrinfo(res);
    inet_ntoa_r(to->sin_addr, ip, 16);
    return 0;
}

static int icmp_open(diag_io_t *io, const char *tool)
{
    int s = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);

    if (s < 0)
        say(io, "%s: sem socket livre (errno %d), tente de novo em alguns segundos\n", tool, errno);
    return s;
}

typedef struct {
    uint8_t  type;               /* ICMP_ER, ICMP_TE or ICMP_DUR */
    uint8_t  code;
    uint8_t  ttl;
    uint32_t us;                 /* round trip time */
    char     from[16];
} reply_t;

/* Sends one echo request and waits up to 1 s for the answer that belongs to
 * it: echo reply, time exceeded (traceroute hop) or destination unreachable.
 * Returns 0 when an answer came, -1 on timeout, -2 on socket error. */
static int probe(int s, const struct sockaddr_in *to, uint16_t id, uint16_t seq, reply_t *r)
{
    uint8_t tx[sizeof(struct icmp_echo_hdr) + DIAG_DATA];
    uint32_t rxw[40];            /* 160 bytes, word aligned: IP + ICMP + inner IP + 8 */
    uint8_t *rx = (uint8_t *)rxw;
    struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)tx;
    int64_t t0;

    memset(tx, 0, sizeof(tx));
    ICMPH_TYPE_SET(e, ICMP_ECHO);
    ICMPH_CODE_SET(e, 0);
    e->id = id;
    e->seqno = lwip_htons(seq);
    for (int i = 0; i < DIAG_DATA; i++)
        tx[sizeof(*e) + i] = 'a' + i % 26;
    e->chksum = inet_chksum(tx, sizeof(tx));

    t0 = esp_timer_get_time();
    if (sendto(s, tx, sizeof(tx), 0, (const struct sockaddr *)to, sizeof(*to)) < 0)
        return -2;

    for (;;) {
        int64_t left = DIAG_WAIT_US - (esp_timer_get_time() - t0);
        struct timeval tv;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n, hl;
        struct ip_hdr *ip;
        struct icmp_echo_hdr *ic, *mine = NULL;

        if (left <= 0)
            return -1;
        tv.tv_sec = left / 1000000;
        tv.tv_usec = left % 1000000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        n = recvfrom(s, rx, sizeof(rxw), 0, (struct sockaddr *)&from, &flen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;
            return -2;
        }

        /* lwIP raw IPv4 sockets deliver the IP header too */
        ip = (struct ip_hdr *)rx;
        hl = IPH_HL(ip) * 4;
        if (n < hl + (int)sizeof(struct icmp_echo_hdr))
            continue;
        ic = (struct icmp_echo_hdr *)(rx + hl);

        if (ICMPH_TYPE(ic) == ICMP_ER) {
            mine = ic;
        } else if (ICMPH_TYPE(ic) == ICMP_TE || ICMPH_TYPE(ic) == ICMP_DUR) {
            /* the router copies our IP header + first 8 bytes of our ICMP */
            int off = hl + 8, ihl;
            struct ip_hdr *in;

            if (n < off + IP_HLEN)
                continue;
            in = (struct ip_hdr *)(rx + off);
            ihl = IPH_HL(in) * 4;
            if (IPH_PROTO(in) != IP_PROTO_ICMP || n < off + ihl + 8)
                continue;
            mine = (struct icmp_echo_hdr *)(rx + off + ihl);
        }
        if (mine == NULL || mine->id != id || mine->seqno != lwip_htons(seq))
            continue;                       /* not ours (other ping, old probe) */

        r->us = (uint32_t)(esp_timer_get_time() - t0);
        r->type = ICMPH_TYPE(ic);
        r->code = ICMPH_CODE(ic);
        r->ttl = IPH_TTL(ip);
        inet_ntoa_r(from.sin_addr, r->from, sizeof(r->from));
        return 0;
    }
}

static uint16_t new_id(void)
{
    return (uint16_t)(esp_timer_get_time() >> 6) ^ 0x5a17;
}

static const char *unreach_mark(uint8_t code)
{
    switch (code) {
    case 0:  return "!N";   /* network unreachable  */
    case 1:  return "!H";   /* host unreachable     */
    case 2:  return "!P";   /* protocol unreachable */
    case 13: return "!X";   /* administratively prohibited */
    default: return "!";
    }
}

void diag_ping(const char *host, int count, diag_out_t out, void *ctx)
{
    diag_io_t io = { .out = out, .ctx = ctx };
    struct sockaddr_in to;
    char ip[16];
    reply_t r;
    uint16_t id = new_id();
    uint32_t min = UINT32_MAX, max = 0;
    uint64_t sum = 0;
    int s, sent = 0, recv = 0;

    if (count < 1) count = 1;
    if (count > DIAG_PING_MAX) count = DIAG_PING_MAX;
    if (resolve4(&io, "ping", host, &to, ip) || (s = icmp_open(&io, "ping")) < 0)
        return;

    say(&io, "PING %s (%s): %d data bytes\n", host, ip, DIAG_DATA);
    for (int seq = 0; seq < count && !io.stop; seq++) {
        int64_t t0 = esp_timer_get_time();
        int rc = probe(s, &to, id, seq, &r);

        sent++;
        if (rc == -2) {
            say(&io, "ping: erro de envio (errno %d)\n", errno);
            break;
        }
        if (rc == 0 && r.type == ICMP_ER) {
            recv++;
            sum += r.us;
            if (r.us < min) min = r.us;
            if (r.us > max) max = r.us;
            say(&io, "%d bytes from %s: seq=%d ttl=%u time=%lu.%03lu ms\n",
                (int)(sizeof(struct icmp_echo_hdr) + DIAG_DATA), r.from, seq, r.ttl,
                (unsigned long)(r.us / 1000), (unsigned long)(r.us % 1000));
        } else if (rc == 0) {
            say(&io, "From %s: seq=%d %s\n", r.from, seq,
                r.type == ICMP_TE ? "Time to live exceeded" : "Destination unreachable");
        } else {
            say(&io, "seq=%d sem resposta (1 s)\n", seq);
        }
        if (seq + 1 < count) {                     /* 1 packet per second */
            int64_t rest = 1000000 - (esp_timer_get_time() - t0);
            if (rest > 0)
                vTaskDelay(pdMS_TO_TICKS(rest / 1000) + 1);
        }
    }
    close(s);

    say(&io, "\n--- %s ping statistics ---\n", host);
    say(&io, "%d packets transmitted, %d packets received, %d%% packet loss\n",
        sent, recv, sent ? (sent - recv) * 100 / sent : 0);
    if (recv) {
        uint32_t avg = (uint32_t)(sum / recv);
        say(&io, "round-trip min/avg/max = %lu.%03lu/%lu.%03lu/%lu.%03lu ms\n",
            (unsigned long)(min / 1000), (unsigned long)(min % 1000),
            (unsigned long)(avg / 1000), (unsigned long)(avg % 1000),
            (unsigned long)(max / 1000), (unsigned long)(max % 1000));
    }
}

void diag_traceroute(const char *host, diag_out_t out, void *ctx)
{
    diag_io_t io = { .out = out, .ctx = ctx };
    struct sockaddr_in to;
    char ip[16];
    reply_t r;
    uint16_t id = new_id();
    int s;

    if (resolve4(&io, "traceroute", host, &to, ip) || (s = icmp_open(&io, "traceroute")) < 0)
        return;

    say(&io, "traceroute to %s (%s), %d hops max, %d byte packets\n",
        host, ip, DIAG_TRACE_HOPS, (int)(IP_HLEN + sizeof(struct icmp_echo_hdr) + DIAG_DATA));
    for (int ttl = 1; ttl <= DIAG_TRACE_HOPS && !io.stop; ttl++) {
        int rc;

        if (setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            say(&io, "traceroute: não foi possível ajustar o TTL (errno %d)\n", errno);
            break;
        }
        rc = probe(s, &to, id, ttl, &r);
        if (rc == -2) {
            say(&io, "traceroute: erro de envio (errno %d)\n", errno);
            break;
        }
        if (rc == -1) {
            say(&io, "%2d  *\n", ttl);
            continue;
        }
        say(&io, "%2d  %s  %lu.%03lu ms%s%s\n", ttl, r.from,
            (unsigned long)(r.us / 1000), (unsigned long)(r.us % 1000),
            r.type == ICMP_DUR ? "  " : "", r.type == ICMP_DUR ? unreach_mark(r.code) : "");
        if (r.type == ICMP_ER || r.type == ICMP_DUR)
            break;                                  /* reached (or refused by) destination */
    }
    close(s);
}

void diag_nslookup(const char *host, diag_out_t out, void *ctx)
{
    diag_io_t io = { .out = out, .ctx = ctx };
    struct addrinfo hints = { 0 }, *res;
    char txt[46];
    int found = 0;
    int64_t t0;

    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t *d = dns_getserver(i);
        if (d && !ip_addr_isany(d) && ipaddr_ntoa_r(d, txt, sizeof(txt)))
            say(&io, "Server:\t\t%s\n", txt);
    }
    say(&io, "\n");

    {   /* lwIP has no reverse (PTR) lookup: an IP literal comes back as is */
        ip_addr_t lit;
        if (ipaddr_aton(host, &lit)) {
            say(&io, "%s é um endereço IP; consulta reversa (PTR) não é suportada no ESP32\n", host);
            return;
        }
    }

    say(&io, "Name:\t\t%s\n", host);
    t0 = esp_timer_get_time();
#if LWIP_IPV4
    hints.ai_family = AF_INET;
    res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        for (struct addrinfo *a = res; a && !io.stop; a = a->ai_next) {
            inet_ntoa_r(((struct sockaddr_in *)a->ai_addr)->sin_addr, txt, sizeof(txt));
            say(&io, "Address:\t%s\n", txt);
            found++;
        }
        freeaddrinfo(res);
    }
#endif
#if LWIP_IPV6
    hints.ai_family = AF_INET6;
    res = NULL;
    if (!io.stop && getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
        for (struct addrinfo *a = res; a && !io.stop; a = a->ai_next) {
            inet6_ntoa_r(((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, txt, sizeof(txt));
            say(&io, "Address:\t%s\n", txt);
            found++;
        }
        freeaddrinfo(res);
    }
#endif
    if (!found)
        say(&io, "*** não foi possível resolver %s (sem resposta ou nome inexistente)\n", host);
    say(&io, "\n(%lu ms)\n", (unsigned long)((esp_timer_get_time() - t0) / 1000));
}
