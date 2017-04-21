// Defines the database schema used to model the filesystem.
// This is intended to be included from sqlfs.c.

SQL_STATEMENT(
  // TODO: document this in more detail
  CREATE TABLE metadata(
    ino INT PRIMARY KEY,  // must be positive. 1 means root.
    mode INT NOT NULL,
    nlink INT NOT NULL,
    uid INT NOT NULL,
    gid INT NOT NULL,
    size INT NOT NULL,
    blksize INT,  // size of filedata blocks for files; NULL for directories
    mtime INT  // in nanoseconds; atime/ctime are not supported
  )
)

SQL_STATEMENT(
  // Partial index allows efficient garbage collection of unlinked metadata.
  CREATE INDEX metadata_unlinked ON metadata(ino) WHERE nlink = 0
)

SQL_STATEMENT(
  // TODO: document this in more detail
  CREATE TABLE filedata(
    ino INT NOT NULL,  // references metadata(ino)
    idx INT NOT NULL,
    data BLOB NOT NULL,
    PRIMARY KEY (ino, idx)
  )
)

SQL_STATEMENT(
  // Table of directory entries.
  //
  // The link to the parent directory (which is reported as "..") has an entry
  // with empty entry_name, to make sure it appears first among all entries.
  //
  // Entry names may not be empty, "." or "..", or contain "/" or "\0".
  CREATE TABLE direntries(
    dir_ino INT NOT NULL,      // references metadata(ino)
    entry_name TEXT NOT NULL,  // see above
    entry_ino INT NOT NULL,    // references metadata(ino)
    entry_mode INT NOT NULL,   // file type bits only (see S_IFMT)
    PRIMARY KEY (dir_ino, entry_name)
  )
)
