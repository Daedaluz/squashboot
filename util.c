#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

void klog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void assert(const char *prefix, int b, ...) {
    if (b) {
        va_list ap;
        va_start(ap, b);
        vprintf(prefix, ap);
        va_end(ap);
        printf(": %s\n", strerror(errno));
        fflush(stdout);
        exit(-1);
    }
}