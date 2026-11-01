/*
 * logger.h - ring-buffered line logger.
 *
 * Records are formatted into a fixed RAM ring (no malloc) and drained to a
 * sink by logger_flush(), the way a UART TX task would drain a log FIFO.
 * Every line is a comma-separated record whose first field is its type:
 *
 *   LOG,<tick_ms>,<LEVEL>,<MODULE>,<message>
 *   TLM,...      (telemetry, written via logger_raw)
 *   ACK,... / ERR,...   (command responses, written via logger_raw)
 *
 * Commas in free text are replaced with ';' so every record stays one CSV row.
 */
#ifndef FW_LOGGER_H
#define FW_LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw/fw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

typedef void (*log_sink_fn)(void *ctx, const char *line);

typedef struct {
    char        lines[LOG_RING_LINES][LOG_LINE_MAX];
    uint16_t    head;      /* index of oldest pending line */
    uint16_t    count;     /* pending lines */
    uint32_t    dropped;   /* lines lost to overflow since last flush */
    uint32_t    last_tick;
    log_level_t min_level;
    log_sink_fn sink;
    void       *sink_ctx;
} logger_t;

void        logger_init(logger_t *lg, log_sink_fn sink, void *sink_ctx, log_level_t min_level);
void        logger_set_level(logger_t *lg, log_level_t level);

/* Returns false if filtered by level or dropped because the ring is full. */
bool        logger_event(logger_t *lg, uint32_t tick_ms, log_level_t level,
                         const char *module, const char *fmt, ...);
bool        logger_vevent(logger_t *lg, uint32_t tick_ms, log_level_t level,
                          const char *module, const char *fmt, va_list ap);

/* Queue a pre-formatted record (type prefix included). */
bool        logger_raw(logger_t *lg, const char *line);

/* Drain pending lines to the sink; returns number of lines written. */
size_t      logger_flush(logger_t *lg);

const char *logger_level_name(log_level_t level);
bool        logger_parse_level(const char *s, log_level_t *out);

/* Replace commas and control characters so text is safe inside one CSV field. */
void        logger_sanitize(char *s);

#ifdef __cplusplus
}
#endif

#endif /* FW_LOGGER_H */
