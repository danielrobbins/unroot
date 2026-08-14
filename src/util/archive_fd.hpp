#pragma once

#include <string>

#include "util/fd.hpp"

namespace util {

UniqueFd reopenArchiveDescriptor(int descriptor, int flags,
                                 std::string& error);

}  // namespace util
