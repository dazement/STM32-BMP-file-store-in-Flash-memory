#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>

/* Recent log lines kept in RAM for the debugger: view debug_log at a
   breakpoint or in Live Watch. debug_log_lines counts every line written;
   the newest is debug_log[(debug_log_lines - 1) % DEBUG_LOG_LINES]. */
#define DEBUG_LOG_LINES 32U
#define DEBUG_LOG_LINE_BYTES 64U

extern char debug_log[DEBUG_LOG_LINES][DEBUG_LOG_LINE_BYTES];
extern volatile uint32_t debug_log_lines;

/* Main-loop context only; interrupt handlers update counters instead. */
void DebugLog_Write(const char *text);
void DebugLog_Printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
