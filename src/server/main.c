#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "protocol.h"

#define MAX_CLIENTS 32
#define INTERVAL_MS 1000   /* push metrics every 1 second */

/* ── graceful shutdown flag ───────────────────────────────────────────────── */
static volatile int running = 1;
static void handle_signal(int sig) { (void)sig; running = 0; }

/* ── /proc/uptime ─────────────────────────────────────────────────────────── */
long read_uptime(void) {
    FILE *fp = fopen("/proc/uptime", "r");
    if (!fp) { perror("open /proc/uptime"); return 0; }
    double up;
    fscanf(fp, "%lf", &up);
    fclose(fp);
    return (long)up;
}

/* ── /proc/meminfo ────────────────────────────────────────────────────────── */
void read_meminfo(SystemMetrics *m) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) { perror("open /proc/meminfo"); return; }

    char key[64];
    long val;
    m->mem_total_kb = m->mem_avail_kb = 0;

    while (fscanf(fp, "%63s %ld kB\n", key, &val) == 2) {
        if (strcmp(key, "MemTotal:") == 0)     m->mem_total_kb = val;
        if (strcmp(key, "MemAvailable:") == 0) m->mem_avail_kb = val;
        if (m->mem_total_kb && m->mem_avail_kb) break;
    }
    fclose(fp);

    if (m->mem_total_kb > 0)
        m->mem_pct = 100.0 * (m->mem_total_kb - m->mem_avail_kb) / m->mem_total_kb;
}

/* ── /proc/loadavg ────────────────────────────────────────────────────────── */
void read_loadavg(SystemMetrics *m) {
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) { perror("open /proc/loadavg"); return; }
    fscanf(fp, "%lf %lf %lf", &m->load_1m, &m->load_5m, &m->load_15m);
    fclose(fp);
}

/* ── /proc/stat (CPU ticks) ───────────────────────────────────────────────── */
typedef struct {
    long long user, nice, system, idle, iowait, irq, softirq;
} CpuTicks;

static int read_cpu_ticks(CpuTicks *cores, int max_cores, int *count) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) { perror("open /proc/stat"); return -1; }

    char line[256];
    *count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] == ' ') continue;

        if (*count >= max_cores) break;
        sscanf(line + 3, "%*d %lld %lld %lld %lld %lld %lld %lld",
               &cores[*count].user,  &cores[*count].nice,
               &cores[*count].system,&cores[*count].idle,
               &cores[*count].iowait,&cores[*count].irq,
               &cores[*count].softirq);
        (*count)++;
    }
    fclose(fp);
    return 0;
}

/* ── delta CPU % ──────────────────────────────────────────────────────────── */
void compute_cpu(SystemMetrics *m,
                 const CpuTicks *t0, const CpuTicks *t1, int count) {
    m->core_count = count;
    double total_pct = 0.0;

    for (int i = 0; i < count; i++) {
        long long busy0 = t0[i].user + t0[i].nice + t0[i].system +
        t0[i].irq  + t0[i].softirq;
        long long busy1 = t1[i].user + t1[i].nice + t1[i].system +
        t1[i].irq  + t1[i].softirq;

        long long idle0 = t0[i].idle + t0[i].iowait;
        long long idle1 = t1[i].idle + t1[i].iowait;

        long long d_busy  = busy1 - busy0;
        long long d_total = (busy1 + idle1) - (busy0 + idle0);

        m->cpu_cores[i] = (d_total > 0) ? 100.0 * d_busy / d_total : 0.0;
        total_pct += m->cpu_cores[i];
    }
    m->cpu_pct = (count > 0) ? total_pct / count : 0.0;
                 }

                 /* ── JSON serializer ──────────────────────────────────────────────────────── */
                 void serialize_metrics(const SystemMetrics *m, char *buf, size_t buf_size) {
                     char cores_str[512] = "[";
                     for (int i = 0; i < m->core_count; i++) {
                         char tmp[32];
                         snprintf(tmp, sizeof(tmp), "%.1f%s", m->cpu_cores[i],
                                  (i < m->core_count - 1) ? "," : "");
                         strncat(cores_str, tmp, sizeof(cores_str) - strlen(cores_str) - 1);
                     }
                     strncat(cores_str, "]", sizeof(cores_str) - strlen(cores_str) - 1);

                     snprintf(buf, buf_size,
                              "{"
                              "\"cpu_pct\":%.1f,"
                              "\"cpu_cores\":%s,"
                              "\"mem_total_kb\":%ld,"
                              "\"mem_avail_kb\":%ld,"
                              "\"mem_pct\":%.1f,"
                              "\"load_1m\":%.2f,"
                              "\"load_5m\":%.2f,"
                              "\"load_15m\":%.2f,"
                              "\"uptime_sec\":%ld"
                              "}\n",
                              m->cpu_pct, cores_str,
                              m->mem_total_kb, m->mem_avail_kb, m->mem_pct,
                              m->load_1m, m->load_5m, m->load_15m,
                              m->uptime_sec
                     );
                 }

                 /* ── TCP server setup ─────────────────────────────────────────────────────── */
                 static int create_server_socket(int port) {
                     int fd = socket(AF_INET, SOCK_STREAM, 0);
                     if (fd < 0) { perror("socket"); return -1; }

                     /* allow immediate reuse after restart */
                     int opt = 1;
                     setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

                     struct sockaddr_in addr = {
                         .sin_family      = AF_INET,
                         .sin_addr.s_addr = INADDR_ANY,
                         .sin_port        = htons(port)
                     };

                     if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                         perror("bind"); close(fd); return -1;
                     }
                     if (listen(fd, 8) < 0) {
                         perror("listen"); close(fd); return -1;
                     }
                     return fd;
                 }

                 /* ── broadcast JSON to all connected clients ──────────────────────────────── */
                 static void broadcast(struct pollfd *fds, int nfds, int server_fd,
                                       const char *buf, int len) {
                     for (int i = 0; i < nfds; i++) {
                         if (fds[i].fd == server_fd || fds[i].fd < 0) continue;
                         if (send(fds[i].fd, buf, len, MSG_NOSIGNAL) < 0) {
                             /* client gone — mark for removal */
                             close(fds[i].fd);
                             fds[i].fd = -1;
                         }
                     }
                                       }

                                       /* ── main ─────────────────────────────────────────────────────────────────── */
                                       int main(void) {
                                           signal(SIGINT,  handle_signal);
                                           signal(SIGTERM, handle_signal);

                                           int server_fd = create_server_socket(PORT);
                                           if (server_fd < 0) return 1;

                                           printf("NixMon server listening on port %d  (Ctrl-C to stop)\n", PORT);

                                           /* pollfd table: slot 0 = server listen socket */
                                           struct pollfd fds[MAX_CLIENTS + 1];
                                           memset(fds, 0, sizeof(fds));
                                           fds[0].fd     = server_fd;
                                           fds[0].events = POLLIN;
                                           int nfds = 1;

                                           /* CPU tick snapshots */
                                           CpuTicks t0[16], t1[16];
                                           int core_count = 0;
                                           read_cpu_ticks(t0, 16, &core_count);

                                           /* timing */
                                           struct timespec last, now;
                                           clock_gettime(CLOCK_MONOTONIC, &last);

                                           while (running) {
                                               /* wait up to INTERVAL_MS for any event */
                                               int ready = poll(fds, nfds, INTERVAL_MS);

                                               /* ── accept new clients ── */
                                               if (ready > 0 && (fds[0].revents & POLLIN)) {
                                                   struct sockaddr_in caddr;
                                                   socklen_t clen = sizeof(caddr);
                                                   int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
                                                   if (cfd >= 0) {
                                                       /* find a free slot */
                                                       int added = 0;
                                                       for (int i = 1; i <= MAX_CLIENTS; i++) {
                                                           if (fds[i].fd <= 0) {
                                                               fds[i].fd     = cfd;
                                                               fds[i].events = POLLIN;
                                                               if (i >= nfds) nfds = i + 1;
                                                               added = 1;
                                                               printf("Client connected  : %s:%d (fd=%d)\n",
                                                                      inet_ntoa(caddr.sin_addr),
                                                                      ntohs(caddr.sin_port), cfd);
                                                               break;
                                                           }
                                                       }
                                                       if (!added) {
                                                           fprintf(stderr, "Max clients reached, dropping connection\n");
                                                           close(cfd);
                                                       }
                                                   }
                                               }

                                               /* ── handle client disconnects (POLLHUP / POLLERR) ── */
                                               for (int i = 1; i < nfds; i++) {
                                                   if (fds[i].fd > 0 && (fds[i].revents & (POLLHUP | POLLERR))) {
                                                       printf("Client disconnected (fd=%d)\n", fds[i].fd);
                                                       close(fds[i].fd);
                                                       fds[i].fd = -1;
                                                   }
                                               }

                                               /* ── check if it's time to push metrics ── */
                                               clock_gettime(CLOCK_MONOTONIC, &now);
                                               long elapsed_ms = (now.tv_sec  - last.tv_sec)  * 1000 +
                                               (now.tv_nsec - last.tv_nsec) / 1000000;

                                               if (elapsed_ms >= INTERVAL_MS) {
                                                   last = now;

                                                   /* collect fresh snapshot */
                                                   read_cpu_ticks(t1, 16, &core_count);

                                                   SystemMetrics m = {0};
                                                   compute_cpu(&m, t0, t1, core_count);
                                                   read_meminfo(&m);
                                                   read_loadavg(&m);
                                                   m.uptime_sec = read_uptime();

                                                   /* rotate snapshots */
                                                   memcpy(t0, t1, sizeof(CpuTicks) * core_count);

                                                   /* serialize and broadcast */
                                                   char json_buf[BUFFER_SIZE];
                                                   serialize_metrics(&m, json_buf, sizeof(json_buf));

                                                   printf("Pushing: cpu=%.1f%% mem=%.1f%% load=%.2f\n",
                                                          m.cpu_pct, m.mem_pct, m.load_1m);

                                                   broadcast(fds, nfds, server_fd, json_buf, strlen(json_buf));
                                               }
                                           }

                                           /* ── cleanup ── */
                                           printf("\nShutting down...\n");
                                           for (int i = 0; i < nfds; i++)
                                               if (fds[i].fd > 0) close(fds[i].fd);

                                               return 0;
                                       }
