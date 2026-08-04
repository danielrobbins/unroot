// Centralized AppException used to propagate util::Error to main's boundary
#pragma once
#include <exception>
#include <string>
#include "util/error_map.hpp"

struct AppException : public std::exception {
  util::Error err;
  std::string ctx;
  std::string msg;
  AppException(util::Error e, std::string context)
    : err(std::move(e)), ctx(std::move(context)) {
      msg = ctx;
      if (!msg.empty()) msg += ": ";
      msg += err.message;
    }
  const char* what() const noexcept override { return msg.c_str(); }
};
