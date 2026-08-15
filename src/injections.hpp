#pragma once

#include <string>
#include <vector>

#include "archive_substitution.hpp"
#include "util/fd.hpp"
#include "util/idmap.hpp"

namespace injections {

enum class Kind { Regular, Symlink };

struct Spec {
  std::string name;
  Kind kind = Kind::Regular;
  std::string source;
  std::string target;
  std::string destination;
  unsigned int uid = 0;
  unsigned int gid = 0;
  mode_t mode = 0644;
  UniqueFd sourceFd;
};

struct Entry {
  std::string name;
  std::string destination;
  std::string original;
  unsigned int uid = 0;
  unsigned int gid = 0;
  mode_t mode = 0644;
};

struct ArchivePlan {
  std::vector<std::string> excludes;
  std::vector<archiveio::Substitution> substitutions;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

std::vector<Spec> defaults(const std::vector<std::string>& disabled = {});
bool parseSpec(const std::string& value, Spec& spec, std::string& error);
bool parseUnpackExclusions(const std::string& value,
                           std::vector<std::string>& disabled,
                           std::string& error);

// Callers hold the managed rootfs operation lock across registry operations.
bool list(const std::string& rootfs, std::vector<Entry>& entries,
          std::string& error);
bool add(const std::string& rootfs, const util::IdMapPlan& idmap,
         std::vector<Spec> specs, std::string& error);
bool remove(const std::string& rootfs, const util::IdMapPlan& idmap,
            const std::vector<std::string>& targets, std::string& error);
bool clear(const std::string& rootfs, const util::IdMapPlan& idmap,
           std::string& error);
ArchivePlan archivePlan(const std::string& rootfs);

}  // namespace injections
