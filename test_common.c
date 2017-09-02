// Unit test framework.

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "test_common.h"

// Number of expectation failures encountered so far.
static int failures;

// Linked list of deferred allocations.
static struct allocation {
  struct allocation *next;
  void *ptr;
} *allocations;

void test_expect_eq(const char *file, int line, const char *func, const char *expr1, const char *expr2, int64_t value1, int64_t value2) {
  if (value1 == value2) return;
  fprintf(stderr, "[%s:%d] Assertion failed in %s(). Expected %s (%" PRId64 ") to be equal to %s (%" PRId64 ").\n",
      file, line, func, expr1, value1, expr2, value2);
  ++failures;
}

void test_fail() {
  ++failures;
}

void defer_free(char *ptr) {
  struct allocation *alloc = calloc(1, sizeof(struct allocation));
  assert(alloc != NULL);
  alloc->next = allocations;
  alloc->ptr = ptr;
  allocations = alloc;
}

void free_deferred() {
  struct allocation *alloc;
  while ((alloc = allocations) != NULL) {
    allocations = alloc->next;
    free(alloc->ptr);
    free(alloc);
  }
}

char *aprintf(const char *format, ...) {
  va_list ap;

  va_start(ap, format);
  int n = vsnprintf(NULL, 0, format, ap);
  va_end(ap);

  assert(n >= 0);
  char *buf = malloc(n + 1);
  assert(buf != NULL);

  va_start(ap, format);
  vsnprintf(buf, n + 1, format, ap);
  va_end(ap);

  return buf;
}

static const struct test_case *find_test(const struct test_case *tests, const char *name) {
  for (const struct test_case *test = tests; test->func != NULL; ++test) {
    if (strcmp(test->name, name) == 0) {
      return test;
    }
  }
  return NULL;
}

static bool run_test(const struct test_case *test) {
  int failures_before = failures;
  test->func();
  bool passed = failures == failures_before;
  fprintf(stderr, "Test %s %s.\n", test->name, passed ? "passed" : "failed");
  return passed;
}

bool test_run(const struct test_case *tests, const char *const *test_names, int num_tests) {
  int failed_tests = 0;
  failures = 0;
  if (num_tests == 0) {
    // Run all tests.
    for (const struct test_case *test = tests; test->func != NULL; ++test) {
      failed_tests += !run_test(test);
    }
  } else {
    for (int i = 0; i < num_tests; ++i) {
      const struct test_case *test = find_test(tests, test_names[i]);
      if (test == NULL) {
        fprintf(stderr, "Test [%s] not found!\n", test_names[i]);
        ++failed_tests;
      } else {
        failed_tests += !run_test(test);
      }
    }
  }
  fprintf(stderr, "%d test failures. %d tests failed.\n", failures, failed_tests);
  return failed_tests == 0;
}
