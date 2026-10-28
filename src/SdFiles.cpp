#include "SdFiles.h"
#include <SD.h>
#include <SD_MMC.h>
#include "TrackList.h"

fs::FS& sdFs() {
  if (SD_MMC.cardType() != CARD_NONE) return SD_MMC;
  return SD;
}

uint64_t sdFreeBytes() {
  if (SD_MMC.cardType() != CARD_NONE) return SD_MMC.totalBytes() - SD_MMC.usedBytes();
  return SD.totalBytes() - SD.usedBytes();
}

std::vector<std::string> listTrackFiles() {
  std::vector<std::string> names;
  File root = sdFs().open("/");
  if (!root) return names;
  for (File e = root.openNextFile(); e; e = root.openNextFile())
    if (!e.isDirectory()) names.push_back(e.name());
  return buildTrackList(names);
}
