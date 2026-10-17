#pragma once

#include <string>

namespace util {

int stageOciLayout(int descriptor, std::string& error);
int decompressOciLayer(int descriptor, int outputDescriptor,
                       const std::string& expectedFilter,
                       std::string& error);

}  // namespace util
