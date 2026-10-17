#include "oci.hpp"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

#include "archive_backend.hpp"
#include "archive_input.hpp"
#include "archive_inspector.hpp"
#include "util/fd.hpp"
#include "util/sha256.hpp"

namespace oci {
namespace {
namespace fs = std::filesystem;

fs::path blobPath(const fs::path& layout, const std::string& digest) {
  return layout / "blobs" / "sha256" / digest.substr(7);
}

std::string layerFilter(const std::string& mediaType) {
  if (mediaType.find("+gzip") != std::string::npos ||
      (mediaType.size() >= 5 &&
       mediaType.compare(mediaType.size() - 5, 5, ".gzip") == 0))
    return "gzip";
  if (mediaType.find("+zstd") != std::string::npos) return "zstd";
  return "none";
}

bool tarFormat(const archiveinfo::ArchiveReport& report) {
  std::string value = report.format;
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value.find("tar") != std::string::npos ||
         value.find("pax") != std::string::npos;
}

uint64_t sum(uint64_t left, uint64_t right) {
  return left <= std::numeric_limits<uint64_t>::max() - right
             ? left + right
             : std::numeric_limits<uint64_t>::max();
}

void addFinding(archiveinfo::Finding& target,
                const archiveinfo::Finding& source) {
  target.count = sum(target.count, source.count);
  for (const auto& example : source.examples) {
    if (target.examples.size() == 4) break;
    if (std::find(target.examples.begin(), target.examples.end(), example) ==
        target.examples.end())
      target.examples.push_back(example);
  }
}

void addReport(archiveinfo::ArchiveReport& target,
               const archiveinfo::ArchiveReport& source) {
  target.members = sum(target.members, source.members);
  target.regularBytes = sum(target.regularBytes, source.regularBytes);
  target.maxUid = std::max(target.maxUid, source.maxUid);
  target.maxGid = std::max(target.maxGid, source.maxGid);
  addFinding(target.unsafePaths, source.unsafePaths);
  addFinding(target.acls, source.acls);
  addFinding(target.xattrs, source.xattrs);
  addFinding(target.capabilities, source.capabilities);
  addFinding(target.selinuxLabels, source.selinuxLabels);
  addFinding(target.sparseFiles, source.sparseFiles);
  addFinding(target.hardLinks, source.hardLinks);
  addFinding(target.symbolicLinks, source.symbolicLinks);
  addFinding(target.devices, source.devices);
  addFinding(target.fifos, source.fifos);
  addFinding(target.sockets, source.sockets);
}

fs::path temporaryStore(std::string& error) {
  std::error_code pathError;
  fs::path parent = fs::temp_directory_path(pathError);
  if (pathError) {
    error = "unable to locate temporary directory for OCI import";
    return {};
  }
  std::string pattern = (parent / "unroot-oci-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char* created = ::mkdtemp(writable.data());
  if (!created) {
    error = "unable to create temporary OCI content store";
    return {};
  }
  fs::path result(created);
  fs::create_directories(result / "layout", pathError);
  if (!pathError)
    fs::create_directories(result / "layers" / "sha256", pathError);
  if (pathError) {
    fs::remove_all(result, pathError);
    error = "unable to initialize temporary OCI content store";
    return {};
  }
  return result;
}

}  // namespace

ImportPlan::ImportPlan(fs::path temporary, Image image,
                       archiveinfo::ArchiveReport report)
    : temporary_(std::move(temporary)),
      image_(std::move(image)),
      report_(std::move(report)) {}

ImportPlan::~ImportPlan() {
  std::error_code error;
  if (!temporary_.empty()) fs::remove_all(temporary_, error);
}

PrepareResult prepare(const archiveio::Input& input,
                      const archive::Backend& backend,
                      const std::string& requestedPlatform) {
  std::string error;
  fs::path temporary = temporaryStore(error);
  if (temporary.empty()) return {nullptr, std::move(error)};
  const auto cleanup = [&temporary]() {
    std::error_code ignored;
    fs::remove_all(temporary, ignored);
  };
  const fs::path layout = temporary / "layout";
  if (!backend.stageOci(input, layout, error)) {
    cleanup();
    return {nullptr, std::move(error)};
  }
  auto selected = loadImage(layout, requestedPlatform);
  if (!selected) {
    cleanup();
    return {nullptr, std::move(selected.error)};
  }

  archiveinfo::ArchiveReport combined;
  combined.format = "OCI image layers";
  for (size_t index = 0; index < selected.image.layers.size(); ++index) {
    auto& layer = selected.image.layers[index];
    archiveio::Input blob(blobPath(layout, layer.descriptor.digest));
    if (!blob) {
      cleanup();
      return {nullptr, blob.error()};
    }
    const fs::path temporaryLayer =
        temporary / "layers" / ("layer-" + std::to_string(index) + ".tmp");
    UniqueFd output(::open(temporaryLayer.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (!output ||
        !backend.decompressLayer(blob, output.get(),
                                 layerFilter(layer.descriptor.mediaType),
                                 error)) {
      cleanup();
      return {nullptr, output ? std::move(error)
                              : "unable to create temporary OCI layer"};
    }
    output.reset();
    const auto digest = util::sha256File(temporaryLayer);
    if (!digest || "sha256:" + digest.hex != layer.diffId) {
      cleanup();
      return {nullptr, "OCI layer diff_id does not match decompressed layer: " +
                           layer.diffId};
    }
    layer.archive = temporary / "layers" / "sha256" / digest.hex;
    std::error_code pathError;
    if (fs::exists(layer.archive, pathError)) {
      fs::remove(temporaryLayer, pathError);
    } else {
      fs::rename(temporaryLayer, layer.archive, pathError);
    }
    if (pathError) {
      cleanup();
      return {nullptr, "unable to publish verified OCI layer"};
    }

    archiveio::Input archive(layer.archive);
    auto inspection = archiveio::Inspector().inspectOciLayer(archive);
    if (!inspection) {
      cleanup();
      return {nullptr, "unable to inspect OCI layer: " + inspection.error};
    }
    layer.report = std::move(inspection.report);
    if (!tarFormat(layer.report)) {
      cleanup();
      return {nullptr, "OCI layer media type does not contain a tar archive"};
    }
    if (layer.report.reservedMetadata || layer.report.unsafePaths.count != 0) {
      cleanup();
      return {nullptr, "OCI layer contains unsafe or reserved rootfs paths"};
    }
    addReport(combined, layer.report);
  }
  return {std::unique_ptr<ImportPlan>(
              new ImportPlan(std::move(temporary), std::move(selected.image),
                             std::move(combined))),
          {}};
}

}  // namespace oci
