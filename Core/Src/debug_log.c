#include "debug_log.h"
#include "main.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char debug_log[DEBUG_LOG_LINES][DEBUG_LOG_LINE_BYTES];
volatile uint32_t debug_log_lines;

/* Each line is "<HAL tick ms> <text>" without the trailing newline. */
void DebugLog_Write(const char *text)
{
  char *line = debug_log[debug_log_lines % DEBUG_LOG_LINES];
  int prefix = snprintf(line, DEBUG_LOG_LINE_BYTES, "%7lu ",
                        (unsigned long)HAL_GetTick());
  if (prefix < 0 || prefix >= (int)DEBUG_LOG_LINE_BYTES)
    prefix = 0;
  size_t length = strcspn(text, "\r\n");
  if (length > DEBUG_LOG_LINE_BYTES - 1U - (size_t)prefix)
    length = DEBUG_LOG_LINE_BYTES - 1U - (size_t)prefix;
  memcpy(line + prefix, text, length);
  line[prefix + length] = '\0';
  ++debug_log_lines;
}

void DebugLog_Printf(const char *format, ...)
{
  char text[DEBUG_LOG_LINE_BYTES];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  DebugLog_Write(text);
}
