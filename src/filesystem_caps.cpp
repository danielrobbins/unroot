#include "filesystem_caps.hpp"

#include <exception>
#include <utility>

#include "nlohmann/json.hpp"

namespace fsinfo {

std::string makeRecord(const FilesystemCaps& caps) {
  const nlohmann::json report = {
      {"posix_acl",
       {{"supported", caps.posixAcl.supported},
        {"detail", caps.posixAcl.detail}}},
      {"xattr",
       {{"supported", caps.xattr.supported}, {"detail", caps.xattr.detail}}}};
  return std::string(Protocol) + " " + report.dump();
}

Result parseRecord(const std::string& record) {
  const std::string prefix = std::string(Protocol) + " ";
  if (record.compare(0, prefix.size(), prefix) != 0)
    return {{}, "unknown filesystem inspection protocol"};
  try {
    const auto report = nlohmann::json::parse(record.substr(prefix.size()));
    if (!report.is_object() || !report.contains("posix_acl") ||
        !report["posix_acl"].is_object() || !report.contains("xattr") ||
        !report["xattr"].is_object())
      return {{}, "invalid filesystem inspection response"};
    const auto& acl = report["posix_acl"];
    const auto& xattr = report["xattr"];
    if (!acl.contains("supported") || !acl["supported"].is_boolean() ||
        !acl.contains("detail") || !acl["detail"].is_string() ||
        !xattr.contains("supported") || !xattr["supported"].is_boolean() ||
        !xattr.contains("detail") || !xattr["detail"].is_string())
      return {{}, "invalid filesystem inspection response"};
    FilesystemCaps caps;
    caps.posixAcl.supported = acl["supported"].get<bool>();
    caps.posixAcl.detail = acl["detail"].get<std::string>();
    caps.xattr.supported = xattr["supported"].get<bool>();
    caps.xattr.detail = xattr["detail"].get<std::string>();
    return {std::move(caps), {}};
  } catch (const std::exception&) {
    return {{}, "invalid filesystem inspection response"};
  }
}

}  // namespace fsinfo
