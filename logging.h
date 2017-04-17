#ifndef LOGGING_H_INCLUDED
#define LOGGING_H_INCLUDED

#define CHECK(expr) \
   do { \
    if (!(expr)) { \
      check_fail(__FILE__, __LINE__, #expr); \
    } \
  } while(0);

void check_fail(const char *file, int line, const char *expr);

#endif /* ndef LOGGING_H_INCLUDED */
