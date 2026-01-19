/* Simple safe_snprintf implementation used by some compilation units.
   Ensures buffer is NUL-terminated and returns the number of characters
   that would have been written (like snprintf). */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

int safe_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (n < 0)
    {
        if (size > 0)
            buf[0] = '\0';
        return -1;
    }
    if ((size_t)n >= size && size > 0)
    {
        buf[size - 1] = '\0';
    }
    return n;
}
