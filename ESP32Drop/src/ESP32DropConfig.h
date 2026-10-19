#pragma once

// One fixed AWDL social channel per build. This is not channel hopping or bonding.
// 5 GHz builds are experimental and require a radio with 5 GHz support.
#ifndef AWDL_RADIO_CHANNEL
#define AWDL_RADIO_CHANNEL 6
#endif

// C5 hardware ACK fix uses a private ABI in m5stack:esp32@3.3.8.
// The pinned build script enables it; unknown SDKs must opt in explicitly.
#ifndef AWDL_C5_BSSID_FILTER
#define AWDL_C5_BSSID_FILTER 0
#endif

// ASCII bytes avoid shell-dependent quoting of compiler string definitions.
// 01 is the SDK's world default; deployment must use the actual country.
#ifndef AWDL_COUNTRY_0
#define AWDL_COUNTRY_0 '0'
#endif
#ifndef AWDL_COUNTRY_1
#define AWDL_COUNTRY_1 '1'
#endif
#if AWDL_RADIO_CHANNEL != 6 && AWDL_RADIO_CHANNEL != 44 && AWDL_RADIO_CHANNEL != 149
#error "AWDL_RADIO_CHANNEL must be 6, 44 or 149"
#endif
