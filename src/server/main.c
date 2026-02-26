#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "protocol.h"

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
        /* skip the aggregate "cpu " line, read per-core "cpu0", "cpu1", … */
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] == ' ') continue;   /* aggregate line */

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
                     /* build cpu_cores array string first */
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
                              m->cpu_pct,
                              cores_str,
                              m->mem_total_kb,
                              m->mem_avail_kb,
                              m->mem_pct,
                              m->load_1m,
                              m->load_5m,
                              m->load_15m,
                              m->uptime_sec
                     );
                 }

                 /* ── main ─────────────────────────────────────────────────────────────────── */
                 int main(void) {
                     printf("NixMon Server — /proc parser test\n");
                     printf("──────────────────────────────────\n");

                     /* Two snapshots 1 second apart for CPU delta */
                     CpuTicks t0[16], t1[16];
                     int core_count = 0;

                     read_cpu_ticks(t0, 16, &core_count);
                     sleep(1);
                     read_cpu_ticks(t1, 16, &core_count);

                     SystemMetrics m = {0};
                     compute_cpu(&m, t0, t1, core_count);
                     read_meminfo(&m);
                     read_loadavg(&m);
                     m.uptime_sec = read_uptime();

                     /* ── print results ── */
                     printf("Cores detected : %d\n",   m.core_count);
                     printf("CPU (avg)      : %.1f%%\n", m.cpu_pct);
                     for (int i = 0; i < m.core_count; i++)
                         printf("  core%-2d       : %.1f%%\n", i, m.cpu_cores[i]);

                     printf("RAM total      : %ld kB\n",  m.mem_total_kb);
                     printf("RAM available  : %ld kB\n",  m.mem_avail_kb);
                     printf("RAM used       : %.1f%%\n",  m.mem_pct);
                     printf("Load avg       : %.2f  %.2f  %.2f  (1m 5m 15m)\n",
                            m.load_1m, m.load_5m, m.load_15m);
                     printf("Uptime         : %ld sec\n", m.uptime_sec);

                     /* ── serialize to JSON ── */
                     char json_buf[BUFFER_SIZE];
                     serialize_metrics(&m, json_buf, sizeof(json_buf));
                     printf("\nJSON frame:\n%s", json_buf);

                     return 0;
                 }
