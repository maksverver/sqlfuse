#!/bin/bash

# Runs basic tests of the CLI tool.

set -e

# For verbose output (useful for debugging), run as: env V=1 ./cli_tests.sh
test "$V" = 1 && set -o xtrace

SQLFUSE=${SQLFUSE:-./sqlfuse}
FUSERMOUNT=${FUSERMOUNT:-$(which fusermount)}

if [ ! -x "${SQLFUSE}" ]; then
  echo "sqlfuse binary (${SQLFUSE}) not found or not executable."
  exit 1
fi

function sqlfuse() {
  "${SQLFUSE}" "$@"
}

if [ ! -x "${FUSERMOUNT}" ]; then
  echo "fusermount binary (${FUSERMOUNT}) not found or not executable."
  exit 1
fi

function mount() {
  CHILD_PID=$(${SQLFUSE} mount --print_pid "$@")
}

function unmount() {
  fusermount -u "$1"
  # fusermount sends a signal to the child process, but does not wait for it to
  # exit. This causes a race condition where the database may still be locked if
  # we try to re-open it immediately after fusermount returns. The command below
  # waits for the child process to exit, which means it has released its locks.
  tail --pid=${CHILD_PID} -f /dev/null
}

function fusermount() {
  "${FUSERMOUNT}" "$@"
}

function cleanup() {
  if [ $? -ne 0 ]; then
    echo 'Test failed! For verbose output, rerun as: env V=1 ./cli_tests.sh'
  fi
  for dir in "${MNTDIR}" "${MNTDIR2}"; do
    if [ -d "${dir}" ]; then
      if mountpoint -q "${dir}"; then
        unmount "${dir}" || echo "Couldn't unmount ${dir}"
      fi
      rmdir "${dir}" || echo "Couldn't remove mount dir ${dir}"
    fi
  done
  rm -f "${DBFILE}" || echo "Couldn't remove db file ${DBFILE}"
  rmdir "${TMPDIR}" || echo "Couldn't remove temp dir ${TMPDIR}"
}

TMPDIR=$(mktemp -d)
trap cleanup EXIT
MNTDIR=${TMPDIR}/mnt
mkdir "${MNTDIR}"
MNTDIR2=${TMPDIR}/mnt2
mkdir "${MNTDIR2}"
DBFILE=${TMPDIR}/db

function create_files() {
  mkdir -p "$1/dir/subdir"
  echo bla >"$1/dir/subdir/file"
  touch "$1/empty"
}

function verify_files() {
  test -f "$1/empty"
  test ! -s "$1/empty"
  test -d "$1/dir"
  test -d "$1/dir/subdir"
  test -s "$1/dir/subdir/file"
  test "$(cat "$1/dir/subdir/file")" = bla
}

#
# Basic tests without a password
#
echo 'Testing without a password.'

# Create a test db
sqlfuse create --no_password "${DBFILE}"

# Mount it
mount --no_password -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"

# Fill it with some test files
create_files "${MNTDIR}"
verify_files "${MNTDIR}"

# Unmount should work
unmount "${MNTDIR}"
! mountpoint -q "${MNTDIR}"
test ! -e "${MNTDIR}"/dir
test ! -e "${MNTDIR}"/empty

# Compaction.
sqlfuse compact --no_password "${DBFILE}"

# Remount readonly. Files should still be there.
mount --no_password -s "${DBFILE}" "${MNTDIR}" -o ro
verify_files "${MNTDIR}"
! touch "${MNTDIR}/newfile" 2>/dev/null

# Can mount the DB twice in readonly mode.
mount --no_password -s "${DBFILE}" "${MNTDIR2}" -o ro
mountpoint -q "${MNTDIR2}"
verify_files "${MNTDIR2}"
unmount "${MNTDIR2}"

# Cannot mount a second time in writable mode.
! mount --no_password -s "${DBFILE}" "${MNTDIR2}" 2>/dev/null
! mountpoint -q "${MNTDIR2}"

unmount "${MNTDIR}"

# Cannot mount twice in writable  mode.
mount --no_password -s "${DBFILE}" "${MNTDIR}"
! mount --no_password -s "${DBFILE}" "${MNTDIR2}" 2>/dev/null
unmount "${MNTDIR}"

rm "${DBFILE}"

#
# Basic tests with a password
#
echo 'Testing with a password.'

# Create a test db
sqlfuse create --insecure_password=password1 "${DBFILE}"

# Mount it
mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"

# Fill it with some test files
create_files "${MNTDIR}"
verify_files "${MNTDIR}"

# Unmount should work
unmount "${MNTDIR}"
! mountpoint -q "${MNTDIR}"
test ! -e "${MNTDIR}"/dir
test ! -e "${MNTDIR}"/empty

# Compaction.
! sqlfuse compact --insecure_password=password2 "${DBFILE}" 2>/dev/null
sqlfuse compact --insecure_password=password1 "${DBFILE}"

# Remount readonly. Files should still be there.
mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR}" -o ro
verify_files "${MNTDIR}"
! touch "${MNTDIR}/newfile" 2>/dev/null

# Can mount the DB twice in readonly mode.
mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR2}" -o ro
mountpoint -q "${MNTDIR2}"
verify_files "${MNTDIR2}"
unmount "${MNTDIR2}"

# Cannot mount a second time in writable mode.
! mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR2}" 2>/dev/null
! mountpoint -q "${MNTDIR2}"

unmount "${MNTDIR}"

# Cannot mount twice in writable mode
mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR}"
! mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR2}" 2>/dev/null
unmount "${MNTDIR}"

# Rekey.
! sqlfuse rekey --old_insecure_password=wrong --new_insecure_password=irrelevant "${DBFILE}" 2>/dev/null
sqlfuse rekey --old_insecure_password=password1 --new_insecure_password=password2 "${DBFILE}"

# Remount after rekeying.
! mount --insecure_password=password1 -s "${DBFILE}" "${MNTDIR}" 2>/dev/null
mount --insecure_password=password2 -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"
verify_files "${MNTDIR}"

unmount "${MNTDIR}"
rm "${DBFILE}"

#
# Backward compatibility: test opening v3 database.
#

mount --insecure_password=test -s testdata/v3.db "${MNTDIR}" -o ro
test "$(cat "${MNTDIR}/hello.txt")" = 'Hello, world!'
unmount "${MNTDIR}"

#
# Backward compatibility: migrate v3 to v4.
#

cp testdata/v3.db "${DBFILE}"
if ! sqlfuse cipher_migrate --insecure_password=test "${DBFILE}"; then
  # If we're using SQLCipher v3, then we cannot migrate to v4.
  # If migration fails for this reason, we won't consider than a test failure.
  sqlfuse cipher_migrate --insecure_password=test "${DBFILE}" 2>&1 | grep -q 'SQLCipher version .* too low'
else
  mount --insecure_password=test -s "${DBFILE}" "${MNTDIR}" -o ro
  test "$(cat "${MNTDIR}/hello.txt")" = 'Hello, world!'
  unmount "${MNTDIR}"
fi

echo 'All tests passed.'
