// util/result.hpp — tiny Result<T,E>
#pragma once

#include <utility>

namespace util {

template <typename T, typename E>
class Result {
  bool ok_;
  T val_{};
  E err_{};
public:
  static Result ok(T v) {
    Result r; r.ok_ = true; r.val_ = std::move(v); return r;
  }
  static Result err(E e) {
    Result r; r.ok_ = false; r.err_ = std::move(e); return r;
  }
  bool ok() const { return ok_; }
  const T& value() const { return val_; }
  T& value() { return val_; }
  const E& error() const { return err_; }
};

// void-specialization helper
template <typename E>
class Result<void, E> {
  bool ok_;
  E err_{};
public:
  static Result success() { Result r; r.ok_ = true; return r; }
  static Result err(E e) { Result r; r.ok_ = false; r.err_ = std::move(e); return r; }
  bool ok() const { return ok_; }
  const E& error() const { return err_; }
};

} // namespace util
