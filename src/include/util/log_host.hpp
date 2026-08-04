// util/log_host.hpp — host-side step logging helpers
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace util {

inline int hostVerboseLevel() {
  const char* v = ::getenv("UNROOT_VERBOSE");
  return (v && *v) ? std::atoi(v) : 0;
}

inline bool hostJsonLog() {
  const char* v = ::getenv("UNROOT_LOG");
  return v && ::strcmp(v, "json") == 0;
}

inline void hostLogStep(const char* name, bool ok, const std::string& note = std::string()) {
  if (hostVerboseLevel() < 1) return;
  if (hostJsonLog()) {
    if (!note.empty()) std::fprintf(stderr, "{\"unroot\":\"step\",\"name\":\"%s\",\"ok\":%s,\"errno\":0,\"errstr\":\"\",\"note\":\"%s\"}\n", name, ok?"true":"false", note.c_str());
    else std::fprintf(stderr, "{\"unroot\":\"step\",\"name\":\"%s\",\"ok\":%s,\"errno\":0,\"errstr\":\"\"}\n", name, ok?"true":"false");
  } else {
    if (!note.empty()) std::fprintf(stderr, "unroot: step %-18s ok=%d -- %s\n", name, ok?1:0, note.c_str());
    else std::fprintf(stderr, "unroot: step %-18s ok=%d\n", name, ok?1:0);
  }
}

} // namespace util
