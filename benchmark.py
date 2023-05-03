#!/usr/bin/env python3

from random import randrange
import os
import sys
from time import time

filesize_in_mib = 100
filesize_in_bytes = filesize_in_mib << 20


def SequentialWriteBenchmark(file):
  chunk = bytes(b & 255 for b in range(1 << 20))
  time_start = time()
  with open(file, 'wb') as f:
    for _ in range(filesize_in_mib):
      f.write(chunk)
  time_elapsed = time() - time_start
  print('Wrote %d MiB in %.2f s (%.2f MiB/s)' % (filesize_in_mib, time_elapsed, filesize_in_mib / time_elapsed))


def RandomReadBenchmark(file):
  num_random_reads = 10**6
  offsets = [randrange(filesize_in_bytes) for _ in range(num_random_reads)]
  time_start = time()
  with open(file, 'rb') as f:
    for offset in offsets:
      f.seek(offset)
      b = f.read(1)
      assert ord(b) == (offset & 255)
  time_elapsed = time() - time_start
  print('Read %d random bytes in %.2f s (%.2f reads/s)' % (num_random_reads, time_elapsed, num_random_reads / time_elapsed))


def RandomWriteBenchmark(file):
  num_random_writes = 10**5
  offsets = [randrange(filesize_in_bytes) for _ in range(num_random_writes)]
  time_start = time()
  byte = bytes(42)
  with open(file, 'rb+') as f:
    for offset in offsets:
      f.seek(offset)
      f.write(byte)
  time_elapsed = time() - time_start
  print('Wrote %d random bytes in %.2f s (%.2f writes/s)' % (num_random_writes, time_elapsed, num_random_writes / time_elapsed))


def Main(argv):
  if len(argv) != 2:
    print('Usage:', argv[0], '<directory>', file=sys.stderr)
    sys.exit(1)

  directory=argv[1]

  if not os.path.isdir(directory):
    print('Not a directory:', directory, file=sys.stderr)
    sys.exit(1)

  file=os.path.join(directory, 'test.dat')
  if os.path.exists(file):
    print('Already exists:', file, file=sys.stderr)
    sys.exit(1)

  try:
    SequentialWriteBenchmark(file)  # required
    RandomReadBenchmark(file)
    RandomWriteBenchmark(file)
  finally:
    os.unlink(file)


if __name__ == '__main__':
  Main(sys.argv)
