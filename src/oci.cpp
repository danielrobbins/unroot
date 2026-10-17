#include "oci.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <utility>

#include "util/fd.hpp"
#include "util/sha256.hpp"

namespace oci {
namespace {
namespace fs = std::filesystem;

constexpr uint64_t MaxJsonSize = 16 * 1024 * 1024;
constexpr size_t MaxManifests = 1024;
constexpr size_t MaxLayers = 4096;

const std::set<std::string> ManifestMediaTypes{
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json"};
const std::set<std::string> ConfigMediaTypes{
    "application/vnd.oci.image.config.v1+json",
    "application/vnd.docker.container.image.v1+json"};
const std::set<std::string> LayerMediaTypes{
    "application/vnd.oci.image.layer.v1.tar",
    "application/vnd.oci.image.layer.v1.tar+gzip",
    "application/vnd.oci.image.layer.v1.tar+zstd",
    "application/vnd.oci.image.layer.nondistributable.v1.tar",
    "application/vnd.oci.image.layer.nondistributable.v1.tar+gzip",
    "application/vnd.oci.image.layer.nondistributable.v1.tar+zstd",
    "application/vnd.docker.image.rootfs.diff.tar",
    "application/vnd.docker.image.rootfs.diff.tar.gzip",
    "application/vnd.docker.image.rootfs.foreign.diff.tar.gzip"};

struct Candidate {
  Image image;
  bool artifact = false;
  std::string error;
};

std::string normalizedArchitecture(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  if (value == "x86_64" || value == "x86-64") return "amd64";
  if (value == "aarch64") return "arm64";
  return value;
}

bool validName(const std::string& value) {
  return !value.empty() && value != "." && value != ".." &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return std::isalnum(character) || character == '_' ||
                  character == '-' || character == '.';
         });
}

bool validDigest(const std::string& value) {
  constexpr const char* prefix = "sha256:";
  if (value.rfind(prefix, 0) != 0 || value.size() != 71) return false;
  return std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

fs::path blobPath(const fs::path& layout, const std::string& digest) {
  return layout / "blobs" / "sha256" / digest.substr(7);
}

bool readText(const fs::path& path, uint64_t limit, std::string& content,
              std::string& error) {
  UniqueFd input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info {};
  if (!input || ::fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size < 0) {
    error = "unable to read OCI document: " + path.filename().string();
    return false;
  }
  if (static_cast<uint64_t>(info.st_size) > limit) {
    error = "OCI JSON document is too large: " + path.filename().string();
    return false;
  }
  content.clear();
  content.reserve(static_cast<size_t>(info.st_size));
  std::array<char, 16 * 1024> buffer {};
  while (true) {
    const ssize_t count = ::read(input.get(), buffer.data(), buffer.size());
    if (count > 0) {
      content.append(buffer.data(), static_cast<size_t>(count));
    } else if (count == 0) {
      return true;
    } else if (errno != EINTR) {
      error = "unable to read OCI document: " + path.filename().string();
      return false;
    }
  }
}

bool readJson(const fs::path& path, nlohmann::json& value,
              std::string& error) {
  std::string content;
  if (!readText(path, MaxJsonSize, content, error)) return false;
  try {
    value = nlohmann::json::parse(content);
  } catch (...) {
    error = "malformed JSON in OCI document: " + path.filename().string();
    return false;
  }
  if (!value.is_object()) {
    error = "OCI document is not a JSON object: " + path.filename().string();
    return false;
  }
  return true;
}

bool descriptor(const nlohmann::json& value, Descriptor& result,
                std::string& error) {
  if (!value.is_object() || !value.contains("mediaType") ||
      !value["mediaType"].is_string() || !value.contains("digest") ||
      !value["digest"].is_string() || !value.contains("size") ||
      !value["size"].is_number_unsigned()) {
    error = "OCI descriptor is missing mediaType, digest, or size";
    return false;
  }
  result = {value["mediaType"].get<std::string>(),
            value["digest"].get<std::string>(),
            value["size"].get<uint64_t>()};
  if (!validDigest(result.digest)) {
    error = "OCI descriptor does not use a lowercase SHA-256 digest";
    return false;
  }
  return true;
}

bool verify(const fs::path& layout, const Descriptor& descriptorValue,
            fs::path& path, std::string& error) {
  path = blobPath(layout, descriptorValue.digest);
  const auto digest = util::sha256File(path);
  if (!digest) {
    error = "missing or unreadable OCI blob: " + descriptorValue.digest;
    return false;
  }
  if (digest.size != descriptorValue.size) {
    error = "OCI descriptor size does not match blob: " +
            descriptorValue.digest;
    return false;
  }
  if ("sha256:" + digest.hex != descriptorValue.digest) {
    error = "OCI descriptor digest does not match blob: " +
            descriptorValue.digest;
    return false;
  }
  return true;
}

bool schemaTwo(const nlohmann::json& value) {
  return value.contains("schemaVersion") &&
         value["schemaVersion"].is_number_integer() &&
         value["schemaVersion"].get<int>() == 2;
}

bool descriptorPlatform(const nlohmann::json& descriptorValue,
                        Platform& platform) {
  if (!descriptorValue.contains("platform")) return false;
  const auto& value = descriptorValue["platform"];
  if (!value.is_object() || !value.contains("os") ||
      !value["os"].is_string() || !value.contains("architecture") ||
      !value["architecture"].is_string())
    return false;
  platform.os = value["os"].get<std::string>();
  platform.architecture = normalizedArchitecture(
      value["architecture"].get<std::string>());
  if (value.contains("variant")) {
    if (!value["variant"].is_string()) return false;
    platform.variant = value["variant"].get<std::string>();
  }
  return validName(platform.os) && validName(platform.architecture) &&
         (platform.variant.empty() || validName(platform.variant));
}

bool samePlatform(const Platform& left, const Platform& right,
                  bool requireVariant) {
  return left.os == right.os && left.architecture == right.architecture &&
         (!requireVariant || left.variant == right.variant);
}

bool knownAttestation(const nlohmann::json& descriptorValue) {
  if (!descriptorValue.contains("annotations") ||
      !descriptorValue["annotations"].is_object())
    return false;
  const auto& annotations = descriptorValue["annotations"];
  const auto found = annotations.find("vnd.docker.reference.type");
  return found != annotations.end() && found->is_string() &&
         found->get<std::string>() == "attestation-manifest";
}

Candidate loadCandidate(const fs::path& layout,
                        const nlohmann::json& descriptorValue) {
  Candidate result;
  if (knownAttestation(descriptorValue)) {
    result.artifact = true;
    return result;
  }
  if (!descriptor(descriptorValue, result.image.manifest, result.error))
    return result;
  if (ManifestMediaTypes.count(result.image.manifest.mediaType) == 0) {
    result.error = "OCI index uses an unsupported manifest media type: " +
                   result.image.manifest.mediaType;
    return result;
  }
  Platform indexPlatform;
  const bool hasIndexPlatform =
      descriptorValue.contains("platform") &&
      descriptorPlatform(descriptorValue, indexPlatform);
  if (descriptorValue.contains("platform") && !hasIndexPlatform) {
    result.error = "OCI manifest descriptor has a malformed platform";
    return result;
  }

  fs::path manifestPath;
  if (!verify(layout, result.image.manifest, manifestPath, result.error))
    return result;
  nlohmann::json manifest;
  if (!readJson(manifestPath, manifest, result.error)) return result;
  if (!schemaTwo(manifest)) {
    result.error = "OCI image manifest must use schemaVersion 2";
    return result;
  }
  if (manifest.contains("mediaType") &&
      (!manifest["mediaType"].is_string() ||
       manifest["mediaType"] != result.image.manifest.mediaType)) {
    result.error = "OCI image manifest media type disagrees with its descriptor";
    return result;
  }
  if (manifest.contains("artifactType")) {
    result.artifact = true;
    result.error.clear();
    return result;
  }
  if (!manifest.contains("config") ||
      !descriptor(manifest["config"], result.image.config, result.error))
    return result;
  if (ConfigMediaTypes.count(result.image.config.mediaType) == 0) {
    result.error = "OCI image uses an unsupported config media type: " +
                   result.image.config.mediaType;
    return result;
  }
  if (!manifest.contains("layers") || !manifest["layers"].is_array() ||
      manifest["layers"].size() > MaxLayers) {
    result.error = "OCI image manifest has no valid layer list";
    return result;
  }
  for (const auto& value : manifest["layers"]) {
    Layer layer;
    if (!descriptor(value, layer.descriptor, result.error)) return result;
    if (LayerMediaTypes.count(layer.descriptor.mediaType) == 0) {
      result.error = "OCI image uses an unsupported layer media type: " +
                     layer.descriptor.mediaType;
      return result;
    }
    result.image.layers.push_back(std::move(layer));
  }

  fs::path configPath;
  if (!verify(layout, result.image.config, configPath, result.error) ||
      !readJson(configPath, result.image.configuration, result.error))
    return result;
  const auto& config = result.image.configuration;
  if (!config.contains("os") || !config["os"].is_string() ||
      !config.contains("architecture") ||
      !config["architecture"].is_string()) {
    result.error = "OCI image config has no operating system or architecture";
    return result;
  }
  result.image.platform.os = config["os"].get<std::string>();
  result.image.platform.architecture = normalizedArchitecture(
      config["architecture"].get<std::string>());
  if (config.contains("variant")) {
    if (!config["variant"].is_string()) {
      result.error = "OCI image config has a malformed architecture variant";
      return result;
    }
    result.image.platform.variant = config["variant"].get<std::string>();
  }
  if (result.image.platform.os != "linux" ||
      !validName(result.image.platform.architecture) ||
      (!result.image.platform.variant.empty() &&
       !validName(result.image.platform.variant))) {
    result.error = "OCI image does not describe a supported Linux platform";
    return result;
  }
  if (hasIndexPlatform) {
    if (indexPlatform.os != result.image.platform.os ||
        indexPlatform.architecture != result.image.platform.architecture ||
        (!indexPlatform.variant.empty() &&
         !result.image.platform.variant.empty() &&
         indexPlatform.variant != result.image.platform.variant)) {
      result.error = "OCI index platform disagrees with the image config";
      return result;
    }
    if (result.image.platform.variant.empty())
      result.image.platform.variant = indexPlatform.variant;
  }
  if (!config.contains("rootfs") || !config["rootfs"].is_object() ||
      !config["rootfs"].contains("type") ||
      config["rootfs"]["type"] != "layers" ||
      !config["rootfs"].contains("diff_ids") ||
      !config["rootfs"]["diff_ids"].is_array() ||
      config["rootfs"]["diff_ids"].size() != result.image.layers.size()) {
    result.error = "OCI image config rootfs does not match the manifest layers";
    return result;
  }
  for (size_t index = 0; index < result.image.layers.size(); ++index) {
    const auto& value = config["rootfs"]["diff_ids"][index];
    if (!value.is_string() || !validDigest(value.get<std::string>())) {
      result.error = "OCI image config contains an invalid layer diff_id";
      return result;
    }
    result.image.layers[index].diffId = value.get<std::string>();
  }
  return result;
}

Platform hostPlatform() {
  struct utsname host {};
  Platform result{"linux", {}, {}};
  if (::uname(&host) == 0)
    result.architecture = normalizedArchitecture(host.machine);
  return result;
}

bool matchesRequest(const Platform& candidate, const Platform& requested) {
  return samePlatform(candidate, requested, !requested.variant.empty());
}

}  // namespace

std::string Platform::string() const {
  std::string result = os + "/" + architecture;
  if (!variant.empty()) result += "/" + variant;
  return result;
}

bool parsePlatform(const std::string& value, Platform& platform) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t end = value.find('/', start);
    parts.push_back(value.substr(
        start, (end == std::string::npos ? value.size() : end) - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if ((parts.size() != 2 && parts.size() != 3) || parts[0] != "linux" ||
      !validName(parts[1]) || (parts.size() == 3 && !validName(parts[2])))
    return false;
  platform = {parts[0], normalizedArchitecture(parts[1]),
              parts.size() == 3 ? parts[2] : std::string{}};
  return true;
}

ImageResult loadImage(const fs::path& layout,
                      const std::string& requestedPlatform) {
  nlohmann::json layoutDocument;
  std::string error;
  if (!readJson(layout / "oci-layout", layoutDocument, error))
    return {{}, std::move(error)};
  if (!layoutDocument.contains("imageLayoutVersion") ||
      layoutDocument["imageLayoutVersion"] != "1.0.0")
    return {{}, "OCI layout must use imageLayoutVersion 1.0.0"};

  nlohmann::json index;
  if (!readJson(layout / "index.json", index, error))
    return {{}, std::move(error)};
  if (!schemaTwo(index) || !index.contains("manifests") ||
      !index["manifests"].is_array() || index["manifests"].empty() ||
      index["manifests"].size() > MaxManifests)
    return {{}, "OCI index has no valid manifest list"};

  Platform requested;
  if (!requestedPlatform.empty() &&
      !parsePlatform(requestedPlatform, requested))
    return {{}, "--platform expects linux/ARCH or linux/ARCH/VARIANT"};

  std::vector<Image> images;
  for (const auto& descriptorValue : index["manifests"]) {
    Candidate candidate = loadCandidate(layout, descriptorValue);
    if (!candidate.error.empty()) return {{}, std::move(candidate.error)};
    if (!candidate.artifact) images.push_back(std::move(candidate.image));
  }
  if (images.empty()) return {{}, "OCI index contains no runnable images"};

  Platform selected = requestedPlatform.empty() ? hostPlatform() : requested;
  std::vector<size_t> matches;
  if (images.size() == 1 && requestedPlatform.empty()) {
    matches.push_back(0);
  } else {
    for (size_t indexValue = 0; indexValue < images.size(); ++indexValue)
      if (matchesRequest(images[indexValue].platform, selected))
        matches.push_back(indexValue);
  }
  if (matches.size() != 1) {
    const std::string choice = requestedPlatform.empty()
        ? "host platform " + selected.string()
        : "requested platform " + requested.string();
    return {{}, "OCI index does not contain exactly one image for " + choice +
                    "; use --platform to select explicitly"};
  }
  Image image = std::move(images[matches.front()]);
  for (const auto& layer : image.layers) {
    fs::path ignored;
    if (!verify(layout, layer.descriptor, ignored, error))
      return {{}, std::move(error)};
  }
  return {std::move(image), {}};
}

nlohmann::json ImportPlan::provenance() const {
  nlohmann::json value{{"mode", "flattened"},
                       {"manifest_digest", image_.manifest.digest},
                       {"config_digest", image_.config.digest},
                       {"platform", image_.platform.string()}};
  nlohmann::json layers = nlohmann::json::array();
  for (const auto& layer : image_.layers)
    layers.push_back({{"digest", layer.descriptor.digest},
                      {"diff_id", layer.diffId}});
  value["layers"] = std::move(layers);
  const auto config = image_.configuration.find("config");
  if (config != image_.configuration.end() && config->is_object()) {
    nlohmann::json runtime = nlohmann::json::object();
    for (const char* field : {"Entrypoint", "Cmd", "Env", "User",
                              "WorkingDir", "ExposedPorts"})
      if (config->contains(field)) runtime[field] = (*config)[field];
    if (!runtime.empty()) value["image_config"] = std::move(runtime);
  }
  return value;
}

}  // namespace oci
