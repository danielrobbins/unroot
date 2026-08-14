#include "archive_report.hpp"

#include <sstream>

namespace archiveinfo {
namespace {

nlohmann::json findingJson(const Finding& finding) {
  return {{"count", finding.count}, {"examples", finding.examples}};
}

bool parseFinding(const nlohmann::json& parent, const char* name,
                  Finding& finding) {
  if (!parent.contains(name) || !parent[name].is_object()) return false;
  const auto& value = parent[name];
  if (!value.contains("count") || !value["count"].is_number_unsigned() ||
      !value.contains("examples") || !value["examples"].is_array())
    return false;
  finding.count = value["count"].get<uint64_t>();
  finding.examples.clear();
  for (const auto& example : value["examples"]) {
    if (!example.is_string() || finding.examples.size() >= 4) return false;
    finding.examples.push_back(example.get<std::string>());
  }
  return true;
}

bool unsignedValue(const nlohmann::json& value, const char* name,
                   uint64_t& output) {
  if (!value.contains(name) || !value[name].is_number_unsigned()) return false;
  output = value[name].get<uint64_t>();
  return true;
}

}  // namespace

nlohmann::json ArchiveReport::toJson() const {
  return {
      {"format", format},
      {"filters", filters},
      {"members", members},
      {"regular_bytes", regularBytes},
      {"max_uid", maxUid},
      {"max_gid", maxGid},
      {"layout",
       {{"reserved_metadata", reservedMetadata}, {"oci", ociLayout}}},
      {"metadata",
       {{"unsafe_paths", findingJson(unsafePaths)},
        {"acls", findingJson(acls)},
        {"xattrs", findingJson(xattrs)},
        {"capabilities", findingJson(capabilities)},
        {"selinux_labels", findingJson(selinuxLabels)},
        {"sparse_files", findingJson(sparseFiles)},
        {"hard_links", findingJson(hardLinks)},
        {"symbolic_links", findingJson(symbolicLinks)},
        {"devices", findingJson(devices)},
        {"fifos", findingJson(fifos)},
        {"sockets", findingJson(sockets)}}}};
}

std::string makeRecord(const ArchiveReport& report) {
  return std::string(Protocol) + " " + report.toJson().dump();
}

ReportResult parseRecord(const std::string& record) {
  const std::string prefix = std::string(Protocol) + " ";
  if (record.compare(0, prefix.size(), prefix) != 0)
    return {{}, "unknown archive inspection protocol"};
  try {
    const auto value = nlohmann::json::parse(record.substr(prefix.size()));
    if (!value.is_object() || !value.contains("format") ||
        !value["format"].is_string() || !value.contains("filters") ||
        !value["filters"].is_array() || !value.contains("layout") ||
        !value["layout"].is_object() || !value.contains("metadata") ||
        !value["metadata"].is_object())
      return {{}, "invalid archive inspection response"};

    ArchiveReport report;
    report.format = value["format"].get<std::string>();
    for (const auto& filter : value["filters"]) {
      if (!filter.is_string())
        return {{}, "invalid archive inspection response"};
      report.filters.push_back(filter.get<std::string>());
    }
    if (!unsignedValue(value, "members", report.members) ||
        !unsignedValue(value, "regular_bytes", report.regularBytes) ||
        !unsignedValue(value, "max_uid", report.maxUid) ||
        !unsignedValue(value, "max_gid", report.maxGid))
      return {{}, "invalid archive inspection response"};

    const auto& layout = value["layout"];
    if (!layout.contains("reserved_metadata") ||
        !layout["reserved_metadata"].is_boolean() ||
        !layout.contains("oci") || !layout["oci"].is_boolean())
      return {{}, "invalid archive inspection response"};
    report.reservedMetadata = layout["reserved_metadata"].get<bool>();
    report.ociLayout = layout["oci"].get<bool>();

    const auto& metadata = value["metadata"];
    if (!parseFinding(metadata, "unsafe_paths", report.unsafePaths) ||
        !parseFinding(metadata, "acls", report.acls) ||
        !parseFinding(metadata, "xattrs", report.xattrs) ||
        !parseFinding(metadata, "capabilities", report.capabilities) ||
        !parseFinding(metadata, "selinux_labels", report.selinuxLabels) ||
        !parseFinding(metadata, "sparse_files", report.sparseFiles) ||
        !parseFinding(metadata, "hard_links", report.hardLinks) ||
        !parseFinding(metadata, "symbolic_links", report.symbolicLinks) ||
        !parseFinding(metadata, "devices", report.devices) ||
        !parseFinding(metadata, "fifos", report.fifos) ||
        !parseFinding(metadata, "sockets", report.sockets))
      return {{}, "invalid archive inspection response"};
    return {std::move(report), {}};
  } catch (const std::exception&) {
    return {{}, "invalid archive inspection response"};
  }
}

}  // namespace archiveinfo
