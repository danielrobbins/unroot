#pragma once

#include <string>
#include <vector>

#include "archive_substitution.hpp"
#include "util/progress_style.hpp"

namespace util {

int unpackArchive(int descriptor, bool force, std::string& message,
                  int progressDescriptor = -1,
                  ProgressStyle progressStyle = ProgressStyle::Ascii);
int applyOciLayer(int descriptor, bool force, std::string& message,
                  int progressDescriptor = -1,
                  ProgressStyle progressStyle = ProgressStyle::Ascii);
int packArchive(int descriptor, const std::string& filter,
                const std::vector<std::string>& excludes,
                const std::vector<std::string>& patterns,
                const std::vector<archiveio::Substitution>& substitutions,
                bool force, bool rootOnly,
                std::string& message, int progressDescriptor = -1,
                ProgressStyle progressStyle = ProgressStyle::Ascii);

}  // namespace util
