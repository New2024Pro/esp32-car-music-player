#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

// Strict decimal integer: optional sign, digits only, no whitespace, no overflow.
// `out` is untouched on failure.
inline bool parseLong(const std::string& s, long& out) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  if (i == s.size()) return false;
  for (size_t k = i; k < s.size(); k++)
    if (s[k] < '0' || s[k] > '9') return false;
  errno = 0;
  long v = strtol(s.c_str(), nullptr, 10);
  if (errno == ERANGE) return false;
  out = v;
  return true;
}

inline int clampVolume(long v) { return v < 0 ? 0 : (v > 100 ? 100 : (int)v); }

// Bluetooth PIN / passkey: 1..maxLen ASCII digits, nothing else (no spaces or sign).
inline bool isDigitCode(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

// Escapes for use inside a JSON string literal. UTF-8 bytes pass through unchanged.
inline std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else o += (char)c;
    }
  }
  return o;
}
