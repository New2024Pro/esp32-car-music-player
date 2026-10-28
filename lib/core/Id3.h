#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Title / artist / album from an ID3v2 tag, as UTF-8. Empty fields when absent.
struct Id3Tags {
  std::string title, artist, album;
};

// Appends code point `cp` to `out` as UTF-8.
inline void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

// ID3 text frame body (encoding byte + text) -> UTF-8. Stops at the first terminator.
//   0 = ISO-8859-1, 1 = UTF-16 with BOM, 2 = UTF-16BE, 3 = UTF-8
inline std::string id3Text(const uint8_t* p, size_t n) {
  std::string out;
  if (n < 1) return out;
  uint8_t enc = p[0];
  p++;
  n--;
  if (enc == 0) {
    for (size_t i = 0; i < n && p[i]; i++) appendUtf8(out, p[i]);
  } else if (enc == 3) {
    for (size_t i = 0; i < n && p[i]; i++) out += (char)p[i];
  } else if (enc == 1 || enc == 2) {
    bool le = false;
    size_t i = 0;
    if (enc == 1 && n >= 2) {
      if (p[0] == 0xFF && p[1] == 0xFE) le = true;
      if ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF)) i = 2;
    }
    for (; i + 1 < n; i += 2) {
      uint32_t u = le ? (p[i] | (p[i + 1] << 8)) : ((p[i] << 8) | p[i + 1]);
      if (u == 0) break;
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {  // surrogate pair
        uint32_t lo = le ? (p[i + 2] | (p[i + 3] << 8)) : ((p[i + 2] << 8) | p[i + 3]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          appendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
          i += 2;
          continue;
        }
      }
      appendUtf8(out, u);
    }
  }
  // Trim trailing spaces (common padding in old taggers).
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Parses the ID3v2 tag at the start of `buf` (the first `n` bytes of the file; a partial tag is
// fine, frames past `n` are ignored). Returns false when there is no ID3v2 tag.
inline bool parseId3v2(const uint8_t* buf, size_t n, Id3Tags& out) {
  if (n < 10 || buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') return false;
  uint8_t ver = buf[3];
  if (ver < 2 || ver > 4) return false;
  if (buf[5] & 0x80) return false;  // unsynchronised tag: rare, not worth decoding
  size_t tagEnd = 10 + (((size_t)buf[6] << 21) | ((size_t)buf[7] << 14) | ((size_t)buf[8] << 7) | buf[9]);
  if (tagEnd > n) tagEnd = n;
  size_t pos = 10;
  if (ver >= 3 && (buf[5] & 0x40) && pos + 4 <= tagEnd) {  // extended header
    size_t ext = ver == 4 ? (((size_t)buf[pos] << 21) | ((size_t)buf[pos + 1] << 14) |
                             ((size_t)buf[pos + 2] << 7) | buf[pos + 3])
                          : (((size_t)buf[pos] << 24) | ((size_t)buf[pos + 1] << 16) |
                             ((size_t)buf[pos + 2] << 8) | buf[pos + 3]) + 4;
    pos += ext;
  }
  const size_t hdr = ver == 2 ? 6 : 10;
  while (pos + hdr <= tagEnd) {
    const uint8_t* f = buf + pos;
    if (f[0] == 0) break;  // padding
    size_t size;
    if (ver == 2) size = ((size_t)f[3] << 16) | ((size_t)f[4] << 8) | f[5];
    else if (ver == 4) size = ((size_t)f[4] << 21) | ((size_t)f[5] << 14) | ((size_t)f[6] << 7) | f[7];
    else size = ((size_t)f[4] << 24) | ((size_t)f[5] << 16) | ((size_t)f[6] << 8) | f[7];
    if (size == 0 || pos + hdr + size > tagEnd) break;
    std::string id(reinterpret_cast<const char*>(f), ver == 2 ? 3 : 4);
    const uint8_t* body = f + hdr;
    if (id == "TIT2" || id == "TT2") out.title = id3Text(body, size);
    else if (id == "TPE1" || id == "TP1") out.artist = id3Text(body, size);
    else if (id == "TALB" || id == "TAL") out.album = id3Text(body, size);
    pos += hdr + size;
  }
  return true;
}

// "Artist - Title.mp3" style file name -> display title (extension removed).
inline std::string titleFromFileName(const std::string& name) {
  size_t start = (!name.empty() && name[0] == '/') ? 1 : 0;
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot < start) dot = name.size();
  return name.substr(start, dot - start);
}

// Naming standard for files without tags: "NN - Artist - Title.mp3". The leading number (play
// order) is dropped; "Artist - Title" is split on the first " - ". Anything else becomes the
// title as a whole. `artist` is left untouched when the name has no artist part.
inline void parseFileName(const std::string& name, std::string& title, std::string& artist) {
  std::string s = titleFromFileName(name);
  size_t i = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
  if (i > 0 && i < s.size()) {  // "01 - ", "01. ", "01_", "01 "
    size_t j = i;
    while (j < s.size() && (s[j] == ' ' || s[j] == '-' || s[j] == '.' || s[j] == '_')) j++;
    if (j > i && j < s.size()) s = s.substr(j);
  }
  size_t dash = s.find(" - ");
  if (dash != std::string::npos && dash > 0 && dash + 3 < s.size()) {
    artist = s.substr(0, dash);
    title = s.substr(dash + 3);
  } else {
    title = s;
  }
}

// Text for head units: drops what older navis cannot draw and may reject the whole title for:
// emoji and other characters outside the Basic Multilingual Plane (4-byte UTF-8), emoji
// variation selectors (U+FE0E/U+FE0F) and zero-width joiners (U+200D). Underscores become
// spaces. Runs of spaces collapse, and leading / trailing spaces are trimmed.
inline std::string navSafeText(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + len > s.size()) break;  // truncated sequence at the end
    std::string ch = s.substr(i, len);
    i += len;
    if (len == 4) continue;                                      // emoji etc.
    if (ch == "\xEF\xB8\x8F" || ch == "\xEF\xB8\x8E") continue;  // U+FE0F / U+FE0E
    if (ch == "\xE2\x80\x8D") continue;                          // U+200D
    if (len == 1 && c >= 0x80) continue;                          // stray byte, not UTF-8
    if (ch == "_") ch = " ";
    if (ch == " " && (out.empty() || out.back() == ' ')) continue;
    out += ch;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Cuts `s` to at most `maxBytes` without splitting a UTF-8 character.
inline std::string utf8Truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
  return s.substr(0, cut);
}
