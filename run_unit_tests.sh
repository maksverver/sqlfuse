#!/bin/sh

set -e

for test in "$@"; do
  echo "Running $test."
  ./"$test"
done
