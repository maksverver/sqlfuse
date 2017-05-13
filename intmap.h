#ifndef INTMAP_H_INCLUDED
#define INTMAP_H_INCLUDED

#include <stdint.h>
#include <stdlib.h>

// An intmap is a mapping of 64-bit integer keys, to 64-bit integer values.
// All values are 0 by default (i.e. there is no distinction between
// non-existent keys and existing keys with a zero value).
struct intmap;

// Creates a new intmap instance.
//
// Each instance is thread-compatible, but not thread-safe. The caller is
// responsible for synchronizing access.
struct intmap *intmap_create();

// Destroys the intmap instance.
void intmap_destroy(struct intmap *intmap);

// Adds `value_add` to the entry with the given `key` and returns the new value.
//
// The amount added may also be zero or negative. (Zero is useful to retrieve
// the current value without making any changes.)
//
// If adding `value_add` to the value currently associated with `key` causes
// integer overflow, the behavior of this function is undefined!
int64_t intmap_update(struct intmap *intmap, int64_t key, int64_t value_add);

// Returns the number of keys with non-zero values in the intmap.
size_t intmap_size(struct intmap *intmap);

#endif /* ndef INTMAP_H_INCLUDED */
