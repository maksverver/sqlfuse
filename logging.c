#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

bool logging_enabled;

void check_fail(const char *file, int line, const char *func, const char *expr) {
  fprintf(stderr, "[%s:%d] %s() CHECK(%s) failed!\n", file, line, func, expr);
  abort();
}
