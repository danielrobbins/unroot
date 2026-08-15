#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace util {

enum class ProgressStyle {
  Ascii,
  AsciiColor,
  Unicode,
  UnicodeColor,
};

constexpr bool progressUsesUnicode(ProgressStyle style) {
  return style == ProgressStyle::Unicode ||
         style == ProgressStyle::UnicodeColor;
}

constexpr bool progressUsesColor(ProgressStyle style) {
  return style == ProgressStyle::AsciiColor ||
         style == ProgressStyle::UnicodeColor;
}

constexpr std::string_view progressStyleName(ProgressStyle style) {
  switch (style) {
    case ProgressStyle::Ascii:
      return "ascii";
    case ProgressStyle::AsciiColor:
      return "ascii-color";
    case ProgressStyle::Unicode:
      return "unicode";
    case ProgressStyle::UnicodeColor:
      return "unicode-color";
  }
  return "ascii";
}

inline std::optional<ProgressStyle> parseProgressStyle(
    std::string_view name) {
  if (name == "ascii") return ProgressStyle::Ascii;
  if (name == "ascii-color") return ProgressStyle::AsciiColor;
  if (name == "unicode") return ProgressStyle::Unicode;
  if (name == "unicode-color") return ProgressStyle::UnicodeColor;
  return std::nullopt;
}

inline ProgressStyle terminalProgressStyle() {
  const char* locale = std::getenv("LC_ALL");
  if (!locale || !*locale) locale = std::getenv("LC_CTYPE");
  if (!locale || !*locale) locale = std::getenv("LANG");
  std::string value = locale ? locale : "";
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  const bool unicode = value.find("utf-8") != std::string::npos ||
                       value.find("utf8") != std::string::npos;
  const char* term = std::getenv("TERM");
  const bool color = !std::getenv("NO_COLOR") &&
                     (!term || std::string_view(term) != "dumb");
  if (unicode)
    return color ? ProgressStyle::UnicodeColor : ProgressStyle::Unicode;
  return color ? ProgressStyle::AsciiColor : ProgressStyle::Ascii;
}

}  // namespace util
