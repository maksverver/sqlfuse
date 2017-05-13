// Unit tests for intmap.

#include "intmap.h"

#include <stdlib.h>
#include <stdint.h>

#include "test_common.h"

static void test_create() {
  struct intmap *intmap = intmap_create();
  EXPECT(intmap != NULL);
  intmap_destroy(intmap);
}

static void test_update() {
  struct intmap *intmap = intmap_create();

  EXPECT_EQ(intmap_update(intmap, 0, 10), 10);
  EXPECT_EQ(intmap_update(intmap, 1, 20), 20);
  EXPECT_EQ(intmap_update(intmap, 2, 30), 30);
  EXPECT_EQ(intmap_update(intmap, INT64_MAX, 40), 40);
  EXPECT_EQ(intmap_update(intmap, INT64_MIN, 50), 50);

  EXPECT_EQ(intmap_update(intmap, 0, -20), -10);
  EXPECT_EQ(intmap_update(intmap, 1, -20), 0);
  EXPECT_EQ(intmap_update(intmap, 2, -20), 10);
  EXPECT_EQ(intmap_update(intmap, INT64_MAX, -20), 20);
  EXPECT_EQ(intmap_update(intmap, INT64_MIN, -20), 30);

  // This assumes two's complement representation.
  EXPECT_EQ(intmap_update(intmap, 1, INT64_MAX), INT64_MAX);
  EXPECT_EQ(intmap_update(intmap, 1, INT64_MIN), -1);
  EXPECT_EQ(intmap_update(intmap, 1, -INT64_MAX), INT64_MIN);

  intmap_destroy(intmap);
}

static void test_size() {
  struct intmap *intmap = intmap_create();

  EXPECT_EQ(intmap_size(intmap), 0);
  EXPECT_EQ(intmap_update(intmap, 0, 1), 1);
  EXPECT_EQ(intmap_update(intmap, 1, 1), 1);
  EXPECT_EQ(intmap_update(intmap, 2, 0), 0);
  EXPECT_EQ(intmap_update(intmap, 3, -1), -1);
  EXPECT_EQ(intmap_update(intmap, 4, -1), -1);

  EXPECT_EQ(intmap_size(intmap), 4);

  EXPECT_EQ(intmap_update(intmap, 0, -1), 0);
  EXPECT_EQ(intmap_update(intmap, 3, 1), 0);

  EXPECT_EQ(intmap_size(intmap), 2);

  EXPECT_EQ(intmap_update(intmap, 1, -1), 0);
  EXPECT_EQ(intmap_update(intmap, 4, 1), 0);

  EXPECT_EQ(intmap_size(intmap), 0);

  intmap_destroy(intmap);
}

static void test_multiple_instances() {
  struct intmap *a = intmap_create();
  struct intmap *b = intmap_create();

  EXPECT_EQ(intmap_update(a, 0, 1), 1);

  EXPECT_EQ(intmap_size(a), 1);
  EXPECT_EQ(intmap_size(b), 0);

  EXPECT_EQ(intmap_update(b, 0, 10), 10);

  EXPECT_EQ(intmap_size(a), 1);
  EXPECT_EQ(intmap_size(b), 1);

  EXPECT_EQ(intmap_update(a, 0, 100), 101);
  EXPECT_EQ(intmap_update(b, 0, 1000), 1010);

  intmap_destroy(a);

  EXPECT_EQ(intmap_update(b, 0, 1), 1011);

  intmap_destroy(b);
}

static const struct test_case tests[] = {
#define TEST(x) {#x, &test_##x}
  TEST(create),
  TEST(update),
  TEST(size),
  TEST(multiple_instances),
#undef TEST
  {NULL, NULL}};

int main(int argc, char *argv[]) {
  return test_run(tests, (const char**)argv + 1, argc - 1) ? 0 : 1;
}
