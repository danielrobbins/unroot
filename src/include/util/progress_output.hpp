#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "util/fd.hpp"
#include "util/progress_style.hpp"

namespace util {

class ProgressOutput {
 public:
  ProgressOutput() {
    if (::isatty(STDERR_FILENO)) {
      style_ = terminalProgressStyle();
      descriptor_.reset(
          ::fcntl(STDERR_FILENO, F_DUPFD, STDERR_FILENO + 1));
    }
  }

  void appendTo(std::vector<std::string>& arguments) const {
    if (!descriptor_) return;
    arguments.push_back("--progress-fd");
    arguments.push_back(std::to_string(descriptor_.get()));
    arguments.push_back("--progress-style");
    arguments.emplace_back(progressStyleName(style_));
  }

 private:
  UniqueFd descriptor_;
  ProgressStyle style_ = ProgressStyle::Ascii;
};

}  // namespace util
