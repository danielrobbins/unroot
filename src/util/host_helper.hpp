#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace util {

struct HostHelperResult {
  int code = -1;
  std::string output;
  std::string error;
  bool truncated = false;

  explicit operator bool() const {
    return code == 0 && error.empty() && !truncated;
  }
};

std::string siblingHostHelper();
HostHelperResult runHostHelper(const std::vector<std::string>& arguments,
                               size_t outputLimit = 4096,
                               const std::string& helperPath = {});

}  // namespace util
