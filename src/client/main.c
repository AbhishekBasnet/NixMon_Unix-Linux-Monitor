#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

#define SERVER_IP "127.0.0.1"

/* ── JSON parser ──────────────────────────────────────────────────────────── */
/* Pulls a double value for a given key from a flat JSON string.
 *  e.g. parse_double(buf, "cpu_pct") → 23.7                                  */
static double parse_double(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0.0;
    p += strlen(search);
    return atof(p);
}

static long parse_long(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    return atol(p);
}

/* Parse cpu_cores array: [31.2,18.4,22.1,19.8] */
static void parse_cpu_cores(const char *json, SystemMetrics *m) {
    const char *p = strstr(json, "\"cpu_cores\":[");
    if (!p) return;
    p += strlen("\"cpu_cores\":[");

    m->core_count = 0;
    while (*p && *p != ']' && m->core_count < 16) {
        m->cpu_cores[m->core_count++] = atof(p);
        /* advance past this number */
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
    }
}

static void parse_metrics(const char *json, SystemMetrics *m) {
    m->cpu_pct      = parse_double(json, "cpu_pct");
    m->mem_total_kb = parse_long  (json, "mem_total_kb");
    m->mem_avail_kb = parse_long  (json, "mem_avail_kb");
    m->mem_pct      = parse_double(json, "mem_pct");
    m->load_1m      = parse_double(json, "load_1m");
    m->load_5m      = parse_double(json, "load_5m");
    m->load_15m     = parse_double(json, "load_15m");
    m->uptime_sec   = parse_long  (json, "uptime_sec");
    parse_cpu_cores(json, m);
}

/* ── TCP connect ──────────────────────────────────────────────────────────── */
static int connect_to_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)
    };
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(void) {
    printf("NixMon Client — connecting to %s:%d\n", SERVER_IP, PORT);

    int fd = connect_to_server(SERVER_IP, PORT);
    if (fd < 0) {
        fprintf(stderr, "Could not connect to server. Is nixmon-server running?\n");
        return 1;
    }
    printf("Connected! Reading metrics stream...\n\n");

    char buf[BUFFER_SIZE];
    int  buf_len = 0;

    while (1) {
        /* read a chunk from the server */
        int n = recv(fd, buf + buf_len, sizeof(buf) - buf_len - 1, 0);
        if (n <= 0) {
            printf("Server disconnected.\n");
            break;
        }
        buf_len += n;
        buf[buf_len] = '\0';

        /* process all complete newline-delimited frames */
        char *line_start = buf;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';   /* terminate the frame */

            SystemMetrics m = {0};
            parse_metrics(line_start, &m);

            /* print parsed values */
            printf("cpu=%.1f%%  mem=%.1f%%  load=%.2f/%.2f/%.2f  uptime=%lds\n",
                   m.cpu_pct, m.mem_pct,
                   m.load_1m, m.load_5m, m.load_15m,
                   m.uptime_sec);
            for (int i = 0; i < m.core_count; i++)
                printf("  core%-2d: %.1f%%\n", i, m.cpu_cores[i]);
            printf("\n");

            line_start = newline + 1;
        }

        /* move remaining partial frame to front of buffer */
        buf_len = strlen(line_start);
        memmove(buf, line_start, buf_len);
    }

    close(fd);
    return 0;
}
