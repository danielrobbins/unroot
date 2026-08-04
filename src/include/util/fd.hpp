// Simple RAII wrapper for POSIX file descriptors
#pragma once
#include <unistd.h>

class UniqueFd {
  int fd_;
public:
  UniqueFd() : fd_(-1) {}
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd(){ if (fd_ >= 0) ::close(fd_); }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) { reset(); fd_ = other.fd_; other.fd_ = -1; }
    return *this;
  }
  void reset(int newFd = -1){ if (fd_ >= 0) ::close(fd_); fd_ = newFd; }
  int release(){ int t = fd_; fd_ = -1; return t; }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  explicit operator bool() const { return valid(); }
};
