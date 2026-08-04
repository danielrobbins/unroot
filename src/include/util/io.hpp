// util/io.hpp — small I/O helpers
#pragma once

#include <unistd.h>
#include <cerrno>
#include <cstddef>

namespace util {

// Write all bytes from buf to fd, retrying on EINTR/short writes.
// Returns true on full success, false on error. On error, errno is set.
inline bool write_all(int fd, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  size_t left = len;
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      // Shouldn't happen for regular fds; treat as error to avoid loop.
      errno = EIO;
      return false;
    }
    p += static_cast<size_t>(n);
    left -= static_cast<size_t>(n);
  }
  return true;
}

// Read up to len bytes, retrying on EINTR. Returns number of bytes read or -1 on error.
inline ssize_t read_n(int fd, void* buf, size_t len) {
  char* p = static_cast<char*>(buf);
  size_t left = len;
  while (true) {
    ssize_t n = ::read(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    return n; // for regular files, a single read is fine; callers can loop if needed
  }
}

// Read exactly len bytes unless EOF/error occurs. Returns true on success.
// On failure (including EOF before len bytes), returns false and leaves errno
// set by the underlying read or sets EIO for short read.
inline bool read_exact(int fd, void* buf, size_t len) {
  char* p = static_cast<char*>(buf);
  size_t left = len;
  while (left > 0) {
    ssize_t n = ::read(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) {
      // EOF before reading len bytes
      errno = EIO;
      return false;
    }
    p += static_cast<size_t>(n);
    left -= static_cast<size_t>(n);
  }
  return true;
}

} // namespace util
