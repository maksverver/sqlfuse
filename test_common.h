#ifndef TEST_COMMON_H_INCLUDED
#define TEST_COMMON_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#if HAVE_MTRACE
#include <mcheck.h>
#define TEST_MTRACE() mtrace()
#else
#define TEST_MTRACE()
#endif

#define EXPECT_EQ(x, y) test_expect_eq(__FILE__, __LINE__, __func__, #x, #y, (x), (y))
#define EXPECT(x) test_expect_eq(__FILE__, __LINE__, __func__, #x, "true", (bool)(x), true)

void test_expect_eq(const char *file, int line, const char *func, const char *expr1, const char *expr2, int64_t value1, int64_t value2);

void test_fail();

// Stores ptr as an allocation to be freed later by a call to test_free_deferred().
void defer_free(char *ptr);

// Frees all allocations registered with defer_free() since the last call to free_deferred().
void free_deferred();

// Each test case has a name, and a function to execute the test case.
// The test code may call functions like test_fail() to register a test failure.
struct test_case {
  const char *name;
  void (*func)(void);
};

// Runs some or all of the given tests.
//
// num_tests gives the length of the array `test_names`. If `num_tests` == 0,
// then all tests are run. Otherwise, only the tests with names in `test_names`
// are run.
//
// Returns true if all tests pass. Returns false if any test failed.
bool test_run(const struct test_case *tests, const char *const *test_names, int num_tests);

#endif /* ndef TEST_COMMON_H_INCLUDED */
