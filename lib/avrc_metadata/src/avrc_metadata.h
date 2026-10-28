#pragma once
#include <stdint.h>

// Track info the AVRCP target hands to the head unit (GetElementAttributes / GetPlayStatus).
// Implemented in the patched btc_avrc.c; safe to call from any task.
#ifdef __cplusplus
extern "C" {
#endif

// Play status values, as in AVRCP (AVRC_PLAYSTATE_*).
#define AVRC_MD_STOPPED 0
#define AVRC_MD_PLAYING 1
#define AVRC_MD_PAUSED  2

// UTF-8 strings; NULL or "" for unknown. Long strings are cut to fit one AVRCP response.
void avrc_md_set_track(const char* title, const char* artist, const char* album, uint32_t track_no,
                       uint32_t num_tracks, uint32_t length_ms);
// pos_ms is the position now; GetPlayStatus extrapolates from it while playing.
void avrc_md_set_play_status(uint8_t status, uint32_t pos_ms);

#ifdef __cplusplus
}
#endif
