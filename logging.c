#include <stdio.h>
#include <stdlib.h>

#include "logging.h"

void check_fail(const char *file, int line, const char *expr) {
  fprintf(stderr, "[%s:%d] CHECK(%s) failed!\n", file, line, expr);
  abort();
}
