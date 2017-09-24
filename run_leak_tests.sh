#!/bin/sh

set -e

MALLOC_TRACE=$(mktemp)
trap 'rm -f "${MALLOC_TRACE}"' EXIT
export MALLOC_TRACE

for test in "$@"; do
  echo "Running $test with malloc-tracing enabled."
  ./"$test"
  if [ -s "$MALLOC_TRACE" ]; then
    mtrace "$MALLOC_TRACE"
  else
    echo "Trace file is empty! Test must be compiled with -DHAVE_MTRACE."
    exit 1
  fi
  rm -f "$MALLOC_TRACE"
done
