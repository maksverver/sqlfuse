#!/bin/bash

# Runs basic tests of the CLI tool.

set -e
# set -o xtrace  # enable this to debug test failures

SQLFUSE=${SQLFUSE:-$(realpath sqlfuse)}
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

function fusermount() {
  "${FUSERMOUNT}" "$@"
}

function cleanup() {
  for dir in "${MNTDIR}" "${MNTDIR2}"; do
    if [ -d "${dir}" ]; then
      if mountpoint -q "${dir}"; then
        fusermount -u "${dir}" || echo "Couldn't unmount ${dir}"
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
sqlfuse mount --no_password -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"

# Fill it with some test files
create_files "${MNTDIR}"
verify_files "${MNTDIR}"

# Unmount should work
fusermount -u "${MNTDIR}"
! mountpoint -q "${MNTDIR}"
test ! -e "${MNTDIR}"/dir
test ! -e "${MNTDIR}"/empty

# Compaction.
sqlfuse compact --no_password "${DBFILE}"

# Remount readonly. Files should still be there.
sqlfuse mount --no_password --readonly -s "${DBFILE}" "${MNTDIR}"
verify_files "${MNTDIR}"
! touch "${MNTDIR}/newfile" 2>/dev/null

# Can't mount the same db at two places at the same time.
#if sqlfuse mount --no_password -s "${DBFILE}" "${MNTDIR2}"; then
#  echo "Mounting database twice succeeded unexpectedly."
#  exit 1
#fi
#! mountpoint-q "${MNTDIR2}"

fusermount -u "${MNTDIR}"
rm "${DBFILE}"

#
# Basic tests with a password
#
echo 'Testing with a password.'

# Create a test db
sqlfuse create --plaintext_password=password1 "${DBFILE}"

# Mount it
sqlfuse mount --plaintext_password=password1 -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"

# Fill it with some test files
create_files "${MNTDIR}"
verify_files "${MNTDIR}"

# Unmount should work
fusermount -u "${MNTDIR}"
! mountpoint -q "${MNTDIR}"
test ! -e "${MNTDIR}"/dir
test ! -e "${MNTDIR}"/empty

# Compaction.
! sqlfuse compact --plaintext_password=password2 "${DBFILE}" 2>/dev/null
sqlfuse compact --plaintext_password=password1 "${DBFILE}"

# Remount readonly. Files should still be there.
sqlfuse mount --plaintext_password=password1 --readonly "${DBFILE}" "${MNTDIR}"
verify_files "${MNTDIR}"
! touch "${MNTDIR}/newfile" 2>/dev/null

# Can't mount the same db at two places at the same time.
#if sqlfuse mount --no_password -s "${DBFILE}" "${MNTDIR2}"; then
#  echo "Mounting database twice succeeded unexpectedly."
#  exit 1
#fi
#! mountpoint-q "${MNTDIR2}"
fusermount -u "${MNTDIR}"

# Rekey.
! sqlfuse rekey --plaintext_password=wrong --new_plaintext_password=irrelevant "${DBFILE}" 2>/dev/null
sqlfuse rekey --plaintext_password=password1 --new_plaintext_password=password2 "${DBFILE}"

# Remount after rekeying.
! sqlfuse mount --plaintext_password=password1 -s "${DBFILE}" "${MNTDIR}" 2>/dev/null
sqlfuse mount --plaintext_password=password2 -s "${DBFILE}" "${MNTDIR}"
mountpoint -q "${MNTDIR}"
verify_files "${MNTDIR}"

fusermount -u "${MNTDIR}"
rm "${DBFILE}"

echo 'All tests passed.'
