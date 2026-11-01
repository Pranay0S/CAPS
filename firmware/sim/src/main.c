/*
 * ivpump_sim - runs the virtual IV pump as a process.
 *
 * Records are written to stdout, one per line; commands are read from stdin,
 * one per line. A reader thread queues stdin lines so the control loop keeps
 * its fixed period regardless of input.
 *
 *   ivpump_sim [--period-ms N] [--seed N] [--fast] [--ticks N] [--header]
 *
 *   --fast     don't sleep between ticks (batch dataset generation); the whole
 *              of stdin is read before the first tick
 *   --ticks N  exit after N ticks (otherwise run until 'quit' or stdin EOF)
 *   --header   print "#TLM,<columns>" first so the stream is self-describing
 *
 * A command line of the form "@<tick_ms> <command>" is held until simulated
 * time reaches tick_ms, so a scenario can be scripted reproducibly:
 *
 *   printf '@0 start\n@5000 attack spike\n' | ivpump_sim --fast --ticks 200 --seed 1
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "sim/sim.h"

#define QUEUE_LEN 32
#define LINE_LEN  128

static struct {
    pthread_mutex_t lock;
    char            lines[QUEUE_LEN][LINE_LEN];
    int             head, count;
    bool            eof;
} g_queue = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *stdin_reader(void *arg)
{
    (void)arg;
    char line[LINE_LEN];
    while (fgets(line, sizeof line, stdin)) {
        pthread_mutex_lock(&g_queue.lock);
        if (g_queue.count < QUEUE_LEN) {
            int idx = (g_queue.head + g_queue.count) % QUEUE_LEN;
            snprintf(g_queue.lines[idx], LINE_LEN, "%s", line);
            g_queue.count++;
        }
        pthread_mutex_unlock(&g_queue.lock);
    }
    pthread_mutex_lock(&g_queue.lock);
    g_queue.eof = true;
    pthread_mutex_unlock(&g_queue.lock);
    return NULL;
}

/* Pops one queued line into out; returns false when the queue is empty. */
static bool queue_pop(char *out, bool *eof)
{
    bool got = false;
    pthread_mutex_lock(&g_queue.lock);
    if (g_queue.count > 0) {
        snprintf(out, LINE_LEN, "%s", g_queue.lines[g_queue.head]);
        g_queue.head = (g_queue.head + 1) % QUEUE_LEN;
        g_queue.count--;
        got = true;
    }
    *eof = g_queue.eof;
    pthread_mutex_unlock(&g_queue.lock);
    return got;
}

/* Commands scheduled with "@<tick_ms>", kept in arrival order. */
#define SCHED_LEN 256
static struct {
    uint32_t at[SCHED_LEN];
    char     lines[SCHED_LEN][LINE_LEN];
    int      count;
} g_sched;

static void dispatch(sim_t *sim, const char *line)
{
    if (line[0] != '@') {
        sim_command(sim, line);
        return;
    }
    char *rest = NULL;
    unsigned long at = strtoul(line + 1, &rest, 10);
    while (*rest == ' ' || *rest == '\t')
        rest++;
    if (at <= sim->millis) {
        sim_command(sim, rest);
    } else if (g_sched.count < SCHED_LEN) {
        g_sched.at[g_sched.count] = (uint32_t)at;
        snprintf(g_sched.lines[g_sched.count], LINE_LEN, "%s", rest);
        g_sched.count++;
    }
}

static void run_due(sim_t *sim)
{
    int kept = 0;
    for (int i = 0; i < g_sched.count; ++i) {
        if (g_sched.at[i] <= sim->millis) {
            sim_command(sim, g_sched.lines[i]);
        } else {
            g_sched.at[kept] = g_sched.at[i];
            memmove(g_sched.lines[kept], g_sched.lines[i], LINE_LEN);
            kept++;
        }
    }
    g_sched.count = kept;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(uint32_t ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000u, (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void stdout_sink(void *ctx, const char *line)
{
    (void)ctx;
    fputs(line, stdout);
    fputc('\n', stdout);
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [--period-ms N] [--seed N] [--fast] [--ticks N] [--header]\n", argv0);
}

int main(int argc, char **argv)
{
    sim_config_t cfg = { .period_ms = 100, .seed = (uint32_t)time(NULL), .sink = stdout_sink };
    bool fast = false, header = false;
    long max_ticks = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc)
            cfg.period_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            cfg.seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
            max_ticks = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--fast") == 0)
            fast = true;
        else if (strcmp(argv[i], "--header") == 0)
            header = true;
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if (cfg.period_ms == 0) {
        usage(argv[0]);
        return 2;
    }

#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    setvbuf(stdout, NULL, _IOFBF, 1 << 14);

    if (header)
        printf("#TLM,%s\n", SIM_TLM_HEADER);

    static sim_t sim;
    sim_init(&sim, &cfg);
    fflush(stdout);

    char line[LINE_LEN];
    bool eof = false;
    if (fast) {
        /* Batch mode: load the whole script up front, no reader thread. */
        while (fgets(line, sizeof line, stdin))
            dispatch(&sim, line);
        fflush(stdout);
    } else {
        pthread_t reader;
        pthread_create(&reader, NULL, stdin_reader, NULL);
        pthread_detach(reader);
    }

    uint64_t next = mono_ms();
    for (long tick = 0; !sim.quit && (max_ticks == 0 || tick < max_ticks); ++tick) {
        if (!fast) {
            while (queue_pop(line, &eof))
                dispatch(&sim, line);
            /* Interactive mode ends when the controlling process closes stdin. */
            if (eof && max_ticks == 0)
                break;
        } else if (max_ticks == 0 && g_sched.count == 0) {
            break; /* batch script exhausted and no tick limit */
        }
        run_due(&sim);

        sim_tick(&sim);
        fflush(stdout);

        if (!fast) {
            next += cfg.period_ms;
            uint64_t now = mono_ms();
            if (next > now)
                sleep_ms((uint32_t)(next - now));
            else
                next = now; /* fell behind; don't try to catch up in a burst */
        }
    }

    fflush(stdout);
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
