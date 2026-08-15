#pragma once

#include <cstdint>
#include <string>

#include "util/fd.hpp"

namespace util {

UniqueFd reopenArchiveDescriptor(int descriptor, int flags,
                                 std::string& error);
uint64_t archiveDescriptorSize(int descriptor);

}  // namespace util
