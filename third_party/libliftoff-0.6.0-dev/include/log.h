#ifndef LOG_H
#define LOG_H

#include <stdbool.h>

#include <libliftoff.h>

#ifdef __GNUC__
#define _LIFTOFF_ATTRIB_PRINTF(start, end) __attribute__((format(printf, start, end)))
#else
#define _LIFTOFF_ATTRIB_PRINTF(start, end)
#endif

bool
log_has(enum liftoff_log_priority priority);

void
liftoff_log(enum liftoff_log_priority priority, const char *format, ...)
_LIFTOFF_ATTRIB_PRINTF(2, 3);

void
liftoff_log_errno(enum liftoff_log_priority priority, const char *msg);

#endif
