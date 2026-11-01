/*
 * cmd.h - text command interpreter (the pump's service/UART console).
 *
 *   start | pause | stop | clear | status
 *   rate <ml/h> | vtbi <ml> | ids on|off | loglevel debug|info|warn|error
 */
#ifndef FW_CMD_H
#define FW_CMD_H

#include <stdbool.h>

#include "fw/pump.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_MSG_MAX 128

typedef enum {
    CMD_OK = 0,
    CMD_ERROR,     /* recognised but rejected (bad arg, wrong state) */
    CMD_UNKNOWN    /* not a firmware command */
} cmd_status_t;

typedef struct {
    cmd_status_t status;
    char         verb[16];
    char         msg[CMD_MSG_MAX];
} cmd_result_t;

cmd_status_t cmd_execute(pump_t *p, const char *line, cmd_result_t *out);

/* Strict float parse: whole token must be a finite number. */
bool         cmd_parse_float(const char *s, float *out);

#ifdef __cplusplus
}
#endif

#endif /* FW_CMD_H */
