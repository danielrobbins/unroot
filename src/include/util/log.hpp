// util/log.hpp — pluggable logging sink (no direct stderr in library)
#pragma once

#include <cstddef>

namespace util {

enum class LogLevel { Info, Warn, Error };

struct StepView {
  const char* name;
  bool ok;
  int err;
  const char* note; // may be nullptr
};

using LogSink = void(*)(LogLevel level, const StepView&);

inline LogSink& globalLogSink() {
  static LogSink sink = nullptr; return sink;
}

inline void setLogSink(LogSink s) { globalLogSink() = s; }

inline void emitLog(LogLevel lvl, const StepView& sv) {
  if (auto s = globalLogSink()) s(lvl, sv);
}

} // namespace util
