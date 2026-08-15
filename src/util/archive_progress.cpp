#include "archive_progress.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string_view>
#include <utility>

#include <archive.h>

namespace util {
namespace {

constexpr size_t BarWidth = 16;
constexpr size_t LabelWidth = 18;
constexpr size_t DefaultTerminalWidth = 80;
constexpr auto RefreshInterval = std::chrono::milliseconds(100);
constexpr std::string_view Accent = "\x1b[36m";
constexpr std::string_view Reset = "\x1b[0m";

uint64_t archiveBytes(struct archive* object, int filter) {
  const la_int64_t value = archive_filter_bytes(object, filter);
  return value > 0 ? static_cast<uint64_t>(value) : 0;
}

size_t terminalWidth(int descriptor) {
  struct winsize size {};
  return ::ioctl(descriptor, TIOCGWINSZ, &size) == 0 && size.ws_col != 0
             ? size.ws_col
             : DefaultTerminalWidth;
}

std::string repeat(std::string_view text, size_t count) {
  std::string output;
  output.reserve(text.size() * count);
  while (count-- != 0) output += text;
  return output;
}

std::string paint(std::string text, bool color) {
  return color && !text.empty()
             ? std::string(Accent) + text + std::string(Reset)
             : text;
}

std::string labelField(const std::string& label, size_t width, bool color) {
  std::string fitted = label;
  if (fitted.size() > width)
    fitted = width > 3 ? fitted.substr(0, width - 3) + "..."
                       : fitted.substr(0, width);
  return paint(fitted, color) + std::string(width - fitted.size(), ' ');
}

std::string dataSize(uint64_t bytes) {
  static constexpr const char* Units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(Units)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << ' '
         << Units[unit];
  return output.str();
}

std::string dataProgress(uint64_t position, uint64_t total) {
  static constexpr const char* Units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double divisor = 1.0;
  size_t unit = 0;
  while (static_cast<double>(total) / divisor >= 1024.0 &&
         unit + 1 < std::size(Units)) {
    divisor *= 1024.0;
    ++unit;
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(unit == 0 ? 0 : 1)
         << static_cast<double>(std::min(position, total)) / divisor << " / "
         << static_cast<double>(total) / divisor << ' ' << Units[unit];
  return output.str();
}

std::string rate(uint64_t bytes, double seconds, bool done) {
  if (seconds <= 0.0 || (!done && seconds <= 0.05)) return "-- MiB/s";
  std::ostringstream output;
  output << std::fixed << std::setprecision(1)
         << static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds
         << " MiB/s";
  return output.str();
}

std::string duration(double seconds) {
  std::ostringstream output;
  if (seconds < 60.0) {
    output << std::fixed << std::setprecision(seconds < 10.0 ? 1 : 0)
           << seconds << 's';
  } else {
    const auto total = static_cast<uint64_t>(seconds);
    output << total / 60 << 'm' << total % 60 << 's';
  }
  return output.str();
}

std::string bar(size_t width, size_t start, size_t filled, bool unicode,
                bool color) {
  const std::string_view full = unicode ? "█" : "=";
  const std::string_view empty = unicode ? "░" : ".";
  return repeat(empty, start) + paint(repeat(full, filled), color) +
         repeat(empty, width - start - filled);
}

}  // namespace

ArchiveProgress::ArchiveProgress(int descriptor, std::string label,
                                 ProgressStyle style, uint64_t total)
    : descriptor_(descriptor),
      label_(std::move(label)),
      total_(total),
      started_(Clock::now()),
      renderedAt_(started_ - RefreshInterval),
      unicode_(progressUsesUnicode(style)),
      color_(progressUsesColor(style)) {
  if (descriptor_ >= 0 && ::isatty(descriptor_))
    render(false, true);
  else
    descriptor_ = -1;
  if (descriptor_ >= 0 && total_ == 0)
    animator_ = std::thread(&ArchiveProgress::animate, this);
}

ArchiveProgress::~ArchiveProgress() {
  stopAnimation();
  if (descriptor_ >= 0 && rendered_ && !finished_) write("\n");
}

void ArchiveProgress::update(struct archive* object) {
  if (descriptor_ < 0 || !object) return;
  const uint64_t position = archiveBytes(object, -1);
  const uint64_t processed = archiveBytes(object, 0);
  if (animator_.joinable()) {
    std::lock_guard<std::mutex> lock(animationMutex_);
    position_ = position;
    processed_ = processed;
  } else {
    position_ = position;
    processed_ = processed;
    render(false, false);
  }
}

void ArchiveProgress::complete() {
  if (descriptor_ < 0 || finished_) return;
  stopAnimation();
  if (total_ != 0) position_ = total_;
  render(true, true);
  write("\n");
  finished_ = true;
}

void ArchiveProgress::render(bool done, bool force) {
  const auto now = Clock::now();
  if (!force && now - renderedAt_ < RefreshInterval) return;
  renderedAt_ = now;

  const double seconds = std::chrono::duration<double>(now - started_).count();
  const size_t columns = terminalWidth(descriptor_);
  std::ostringstream line;
  line << '\r' << "\x1b[2K";

  if (done) {
    const std::string size =
        dataSize(processed_ != 0 ? processed_ : position_);
    const std::string elapsed = "in " + duration(seconds);
    const std::string speed = rate(processed_, seconds, true);
    const size_t base = LabelWidth + 2 + 4;
    if (columns < base) {
      const std::string compact = label_ + "  done";
      line << compact.substr(0, columns);
    } else {
      line << labelField(label_, LabelWidth, color_) << "  "
           << paint("done", color_);
      if (base + 2 + size.size() <= columns) line << "  " << size;
      if (base + 4 + size.size() + elapsed.size() <= columns)
        line << "  " << elapsed;
      if (base + 6 + size.size() + elapsed.size() + speed.size() <= columns)
        line << "  " << speed;
    }
  } else {
    const bool determinate = total_ != 0;
    double fraction = 0.0;
    std::ostringstream state;
    if (determinate) {
      fraction = std::min(
          1.0, static_cast<double>(position_) / static_cast<double>(total_));
      state << std::setw(3) << static_cast<int>(fraction * 100.0) << '%';
    } else {
      state << dataSize(processed_);
    }
    const std::string amount =
        determinate ? dataProgress(position_, total_) : std::string();
    const std::string speed = rate(processed_, seconds, false);
    const size_t base = LabelWidth + 2 + BarWidth + 2 + state.str().size();

    if (base <= columns) {
      size_t start = 0;
      size_t filled = 0;
      if (determinate) {
        filled = std::min(BarWidth,
                          static_cast<size_t>(fraction * BarWidth));
      } else {
        filled = 5;
        const size_t span = BarWidth - filled;
        const auto ticks =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started_)
                .count() /
            RefreshInterval.count();
        const size_t cycle =
            span == 0 ? 0 : static_cast<size_t>(ticks) % (span * 2);
        start = cycle <= span ? cycle : span * 2 - cycle;
      }
      line << labelField(label_, LabelWidth, color_) << "  "
           << bar(BarWidth, start, filled, unicode_, color_) << "  "
           << state.str();
      const bool showAmount =
          !amount.empty() && base + 4 + amount.size() + speed.size() <= columns;
      if (showAmount)
        line << "  " << amount;
      if (base + 2 + speed.size() + (showAmount ? 2 + amount.size() : 0) <=
          columns)
        line << "  " << speed;
    } else {
      const std::string compact = label_ + "  " + state.str();
      line << compact.substr(0, columns);
    }
  }
  write(line.str());
  rendered_ = true;
}

void ArchiveProgress::animate() {
  std::unique_lock<std::mutex> lock(animationMutex_);
  while (!animationWake_.wait_for(lock, RefreshInterval,
                                  [this] { return stopping_; })) {
    render(false, true);
  }
}

void ArchiveProgress::stopAnimation() {
  if (!animator_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(animationMutex_);
    stopping_ = true;
  }
  animationWake_.notify_one();
  animator_.join();
}

void ArchiveProgress::write(const std::string& text) const {
  const char* data = text.data();
  size_t remaining = text.size();
  while (remaining != 0) {
    const ssize_t written = ::write(descriptor_, data, remaining);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    data += written;
    remaining -= static_cast<size_t>(written);
  }
}

}  // namespace util
