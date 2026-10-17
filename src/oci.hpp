#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "archive_report.hpp"
#include "nlohmann/json.hpp"

namespace archive {
class Backend;
}
namespace archiveio {
class Input;
}

namespace oci {

struct Platform {
  std::string os;
  std::string architecture;
  std::string variant;

  std::string string() const;
};

struct Descriptor {
  std::string mediaType;
  std::string digest;
  uint64_t size = 0;
};

struct Layer {
  Descriptor descriptor;
  std::string diffId;
  std::filesystem::path archive;
  archiveinfo::ArchiveReport report;
};

struct Image {
  Descriptor manifest;
  Descriptor config;
  Platform platform;
  std::vector<Layer> layers;
  nlohmann::json configuration;
};

struct ImageResult {
  Image image;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

bool parsePlatform(const std::string& value, Platform& platform);
ImageResult loadImage(const std::filesystem::path& layout,
                      const std::string& requestedPlatform = {});

struct PrepareResult;

class ImportPlan {
 public:
  ImportPlan(const ImportPlan&) = delete;
  ImportPlan& operator=(const ImportPlan&) = delete;
  ~ImportPlan();

  const Image& image() const { return image_; }
  const archiveinfo::ArchiveReport& report() const { return report_; }
  nlohmann::json provenance() const;

 private:
  friend struct PrepareResult;
  friend PrepareResult prepare(const archiveio::Input&, const archive::Backend&,
                               const std::string&);

  ImportPlan(std::filesystem::path temporary, Image image,
             archiveinfo::ArchiveReport report);

  std::filesystem::path temporary_;
  Image image_;
  archiveinfo::ArchiveReport report_;
};

struct PrepareResult {
  std::unique_ptr<ImportPlan> plan;
  std::string error;

  explicit operator bool() const { return plan && error.empty(); }
};

PrepareResult prepare(const archiveio::Input& input,
                      const archive::Backend& backend,
                      const std::string& requestedPlatform = {});

}  // namespace oci
