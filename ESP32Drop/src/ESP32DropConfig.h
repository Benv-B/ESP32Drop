#pragma once

// Optional measured experiment, not an assertion that iOS will omit video.
// OpenDrop config.py names 0x40 SUPPORTS_IRIS. Keep every other capability bit.
// One shared string is used by both AWDL service TLVs and mDNS TXT records.
#if defined(AWDL_AIRDROP_NO_IRIS) && AWDL_AIRDROP_NO_IRIS
#define AWDL_RECEIVER_FLAGS_TEXT "flags=955"
#define AWDL_RECEIVER_FLAGS_VALUE 955
#else
#define AWDL_RECEIVER_FLAGS_TEXT "flags=1019"
#define AWDL_RECEIVER_FLAGS_VALUE 1019
#endif

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

// The 16 KiB TCP-window benchmark can burst more ACKs between AWDL windows.
// Keep the original queue by default; extra C5 queue storage lives in PSRAM.
#ifndef AWDL_NETIF_TX_QUEUE_DEPTH
#define AWDL_NETIF_TX_QUEUE_DEPTH 16
#endif
#if AWDL_NETIF_TX_QUEUE_DEPTH != 16 && AWDL_NETIF_TX_QUEUE_DEPTH != 32
#error "AWDL_NETIF_TX_QUEUE_DEPTH must be 16 or 32"
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
