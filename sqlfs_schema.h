// Defines the database schema used to model the filesystem.
// This is intended to be included from sqlfs.c.

SQL_STATEMENT(
  // Metadata table stores inode attributes. This is what's returned by stat().
  //
  // Since the ino column is not an AUTOINCREMENT column, ino numbers may be
  // reused over the lifetime of the database.
  //
  // Mode indicates the file type and permissions. The only supported file types
  // are regular files (S_IFREG) and directories (S_IFDIR).
  CREATE TABLE metadata(
    ino INTEGER PRIMARY KEY NOT NULL,  // must be positive. 1 means root.
    mode INTEGER NOT NULL,
    nlink INTEGER NOT NULL,  // for directories: number of subdirectories + 2
    uid INTEGER NOT NULL,
    gid INTEGER NOT NULL,
    size INTEGER,  // size of file; NULL for directories
    blksize INTEGER,  // size of filedata blocks for files; NULL for directories
    mtime INTEGER NOT NULL // in nanoseconds; atime/ctime are not supported
  )
)

SQL_STATEMENT(
  // Partial index allows efficient garbage collection of unlinked metadata.
  CREATE INDEX metadata_unlinked ON metadata(ino) WHERE nlink = 0
)

SQL_STATEMENT(
  // Stores the data contents of file entries.
  //
  // The size and blksize columns determine how data is distributed over rows.
  // The first `floor(size / blksize)` rows contain blksize bytes each. If
  // `size` is not an integer multiple of `blksize, then there is an additional
  // block containing the remaining `size % blksize` bytes. This means that no
  // rows are stored for empty files!
  CREATE TABLE filedata(
    ino INTEGER NOT NULL,  // references metadata(ino)
    idx INTEGER NOT NULL,  // 0-based
    data BLOB NOT NULL,
    PRIMARY KEY (ino, idx)
  )
  // TODO: should this be declared WITHOUT ROWID for efficiency?
)

SQL_STATEMENT(
  // Table of directory entries.
  //
  // The link to the parent directory (which is reported as "..") has an entry
  // with empty entry_name, to make sure it appears first among all entries.
  //
  // Entry names may not be empty, "." or "..", or contain "/" or "\0".
  CREATE TABLE direntries(
    dir_ino INTEGER NOT NULL,      // references metadata(ino)
    entry_name TEXT NOT NULL,      // see above
    entry_ino INTEGER NOT NULL,    // references metadata(ino)
    entry_type INTEGER NOT NULL,   // file type bits (mode >> 12)
    PRIMARY KEY (dir_ino, entry_name)
  )
  // TODO: should this be declared WITHOUT ROWID for efficiency?
)
