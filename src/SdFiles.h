#pragma once
#include <FS.h>
#include <string>
#include <vector>

// The mounted SD card: the on-board slot (SD_MMC) if it has a card, else the SPI module.
fs::FS& sdFs();
// Playable MP3s in the card's root folder, sorted (TrackList.h rules).
std::vector<std::string> listTrackFiles();
// Free bytes on the card. Scans the FAT: slow on big cards, so never on the async_tcp task.
uint64_t sdFreeBytes();
