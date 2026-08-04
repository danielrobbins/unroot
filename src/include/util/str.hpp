// util/str.hpp — small string helpers shared across TUs
#pragma once

#include <string>
#include <cstdio>

namespace util {

inline std::string jsonEscape(const std::string& in) {
  std::string out; out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) { char buf[7]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
        else out += static_cast<char>(c);
    }
  }
  return out;
}

inline std::string q(const std::string& s) { return std::string("\"") + jsonEscape(s) + "\""; }

inline std::string joinArgs(const std::vector<std::string>& v) {
  std::string s; s.reserve(16 * v.size());
  for (size_t i = 0; i < v.size(); ++i) { if (i) s.push_back(' '); s += v[i]; }
  return s;
}

} // namespace util
