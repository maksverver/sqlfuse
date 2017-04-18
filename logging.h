#ifndef LOGGING_H_INCLUDED
#define LOGGING_H_INCLUDED

#include <stdbool.h>
#include <stdio.h>

#define CHECK(expr) \
   do { \
    if (!(expr)) { \
      check_fail(__FILE__, __LINE__, #expr); \
    } \
  } while (false)

#define LOG(...) \
  do { \
    if (logging_enabled) { \
      fprintf(stderr, __VA_ARGS__); \
    } \
  } while (false)

void check_fail(const char *file, int line, const char *expr);

extern bool logging_enabled;

#endif /* ndef LOGGING_H_INCLUDED */
