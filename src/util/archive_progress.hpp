#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "util/progress_style.hpp"

struct archive;

namespace util {

class ArchiveProgress {
 public:
  ArchiveProgress(int descriptor, std::string label, ProgressStyle style,
                  uint64_t total = 0);
  ~ArchiveProgress();

  void update(struct archive* object);
  void complete();

 private:
  using Clock = std::chrono::steady_clock;

  void render(bool done, bool force);
  void animate();
  void stopAnimation();
  void write(const std::string& text) const;

  int descriptor_ = -1;
  std::string label_;
  uint64_t total_ = 0;
  uint64_t position_ = 0;
  uint64_t processed_ = 0;
  Clock::time_point started_;
  Clock::time_point renderedAt_;
  std::mutex animationMutex_;
  std::condition_variable animationWake_;
  std::thread animator_;
  bool stopping_ = false;
  bool unicode_ = false;
  bool color_ = false;
  bool rendered_ = false;
  bool finished_ = false;
};

}  // namespace util
