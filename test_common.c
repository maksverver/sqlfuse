// Unit test framework.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "test_common.h"

static int failures;

void test_expect_eq(const char *file, int line, const char *func, const char *expr1, const char *expr2, int value1, int value2) {
  if (value1 == value2) return;
  fprintf(stderr, "[%s:%d] Assertion failed in %s(). Expected %s (%d) to be equal to %s (%d).\n",
      file, line, func, expr1, value1, expr2, value2);
  ++failures;
}

void test_fail() {
  ++failures;
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
