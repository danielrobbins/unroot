#pragma once

#include <string>

namespace util {

int unpackArchive(int descriptor, bool force, std::string& message);
int packArchive(int descriptor, const std::string& filter, bool force,
                std::string& message);

}  // namespace util
