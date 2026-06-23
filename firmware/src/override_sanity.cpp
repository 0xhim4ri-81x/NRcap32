// src/override_sanity.cpp
#include <stdint.h>

extern "C" int ieee80211_raw_frame_sanity_check(uint32_t frame_ctrl, int len, int type) {
    return 0;   // Bypass Espressif's deauth/disassoc sanity check
}