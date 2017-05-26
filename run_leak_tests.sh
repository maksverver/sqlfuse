#!/bin/sh

set -e

MALLOC_TRACE=$(mktemp)
trap 'rm -f "${MALLOC_TRACE}"' EXIT
export MALLOC_TRACE

for test in "$@"; do
  ./"${test}"
  mtrace "$MALLOC_TRACE"
done
