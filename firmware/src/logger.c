#include "fw/logger.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const LEVEL_NAMES[] = { "DEBUG", "INFO", "WARN", "ERROR" };

void logger_init(logger_t *lg, log_sink_fn sink, void *sink_ctx, log_level_t min_level)
{
    memset(lg, 0, sizeof(*lg));
    lg->sink      = sink;
    lg->sink_ctx  = sink_ctx;
    lg->min_level = min_level;
}

void logger_set_level(logger_t *lg, log_level_t level)
{
    lg->min_level = level;
}

const char *logger_level_name(log_level_t level)
{
    return (level <= LOG_ERROR) ? LEVEL_NAMES[level] : "?";
}

bool logger_parse_level(const char *s, log_level_t *out)
{
    for (int i = LOG_DEBUG; i <= LOG_ERROR; ++i) {
        if (strcasecmp(s, LEVEL_NAMES[i]) == 0) {
            *out = (log_level_t)i;
            return true;
        }
    }
    return false;
}

void logger_sanitize(char *s)
{
    for (; *s; ++s) {
        if (*s == ',')
            *s = ';';
        else if ((unsigned char)*s < 0x20)
            *s = ' ';
    }
}

/* Reserve the next free slot, or NULL (and count a drop) if full. */
static char *ring_reserve(logger_t *lg)
{
    if (lg->count >= LOG_RING_LINES) {
        lg->dropped++;
        return NULL;
    }
    uint16_t idx = (uint16_t)((lg->head + lg->count) % LOG_RING_LINES);
    lg->count++;
    return lg->lines[idx];
}

bool logger_vevent(logger_t *lg, uint32_t tick_ms, log_level_t level,
                   const char *module, const char *fmt, va_list ap)
{
    lg->last_tick = tick_ms;
    if (level < lg->min_level)
        return false;

    char *slot = ring_reserve(lg);
    if (!slot)
        return false;

    char msg[LOG_LINE_MAX];
    vsnprintf(msg, sizeof msg, fmt, ap);
    logger_sanitize(msg);

    char mod[16];
    snprintf(mod, sizeof mod, "%s", module);
    logger_sanitize(mod);

    snprintf(slot, LOG_LINE_MAX, "LOG,%lu,%s,%s,%s",
             (unsigned long)tick_ms, logger_level_name(level), mod, msg);
    return true;
}

bool logger_event(logger_t *lg, uint32_t tick_ms, log_level_t level,
                  const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = logger_vevent(lg, tick_ms, level, module, fmt, ap);
    va_end(ap);
    return ok;
}

bool logger_raw(logger_t *lg, const char *line)
{
    char *slot = ring_reserve(lg);
    if (!slot)
        return false;
    snprintf(slot, LOG_LINE_MAX, "%s", line);
    return true;
}

size_t logger_flush(logger_t *lg)
{
    size_t n = 0;
    while (lg->count > 0) {
        if (lg->sink)
            lg->sink(lg->sink_ctx, lg->lines[lg->head]);
        lg->head = (uint16_t)((lg->head + 1) % LOG_RING_LINES);
        lg->count--;
        n++;
    }
    if (lg->dropped > 0) {
        char line[LOG_LINE_MAX];
        snprintf(line, sizeof line, "LOG,%lu,WARN,LOG,ring overflow: %lu lines dropped",
                 (unsigned long)lg->last_tick, (unsigned long)lg->dropped);
        lg->dropped = 0;
        if (lg->sink)
            lg->sink(lg->sink_ctx, line);
        n++;
    }
    return n;
}
