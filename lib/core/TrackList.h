#pragma once
#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// SD root entry -> playable candidate: ends in .mp3 (any case), not a dot file (macOS ._*).
// Handles potential leading slash from VFS (e.g. "/track.mp3").
inline bool isTrackName(const std::string& n) {
  size_t start = 0;
  if (!n.empty() && n[0] == '/') start = 1;
  if (n.size() - start < 5 || n[start] == '.') return false;
  std::string ext = n.substr(n.size() - 4);
  for (auto& c : ext) c = lowerAscii(c);
  return ext == ".mp3";
}

// Case-insensitive (ASCII) order, ties broken bytewise. Non-ASCII bytes compare as raw bytes.
inline bool trackLess(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    unsigned char x = (unsigned char)lowerAscii(a[i]), y = (unsigned char)lowerAscii(b[i]);
    if (x != y) return x < y;
  }
  if (a.size() != b.size()) return a.size() < b.size();
  return a < b;
}

inline std::vector<std::string> buildTrackList(const std::vector<std::string>& dirNames) {
  std::vector<std::string> out;
  for (auto& n : dirNames)
    if (isTrackName(n)) out.push_back(n);
  std::sort(out.begin(), out.end(), trackLess);
  return out;
}

struct Page {
  size_t total = 0, offset = 0;
  std::vector<std::pair<size_t, std::string>> items;
};

// limit clamped to 1..50; negative offset -> 0; offset >= total -> empty items.
inline Page pageTracks(const std::vector<std::string>& t, long offset, long limit) {
  Page p;
  p.total = t.size();
  if (offset < 0) offset = 0;
  if (limit < 1) limit = 1;
  if (limit > 50) limit = 50;
  p.offset = (size_t)offset;
  for (size_t i = p.offset; i < t.size() && p.items.size() < (size_t)limit; i++) p.items.emplace_back(i, t[i]);
  return p;
}

// No wrap-around: end of list -> -1 (stop); PREV on the first track stays put.
inline int nextIndex(int cur, size_t total) { return (cur + 1 >= 0 && (size_t)(cur + 1) < total) ? cur + 1 : -1; }
inline int prevIndex(int cur, size_t total) {
  if (total == 0) return -1;
  return cur > 0 ? cur - 1 : 0;
}

// First playable index from `start` stepping by dir (+1/-1). No wrap, so at most `total`
// probes; returns -1 if none (caller reports "no playable tracks" or end of list).
inline int skipPolicy(int start, int dir, size_t total, const std::function<bool(int)>& playable) {
  for (int i = start; i >= 0 && (size_t)i < total; i += dir)
    if (playable(i)) return i;
  return -1;
}

// Apostrophe variants (` ' and U+2019) compare equal; otherwise exact, case-sensitive.
inline std::string normalizeName(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '`') o += '\'';
    else if (i + 2 < s.size() + 0 && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
             (unsigned char)s[i + 2] == 0x99) {
      o += '\'';
      i += 2;
    } else o += s[i];
  }
  return o;
}
inline bool namesMatch(const std::string& want, const std::string& seen) {
  return !want.empty() && normalizeName(want) == normalizeName(seen);
}

