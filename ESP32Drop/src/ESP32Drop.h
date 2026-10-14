/* ESP32Drop.h -- AirDrop on the ESP32-S3: receive files from an iPhone or a Mac.
 *
 * Include this one header. It pulls in ESP32AWDL.h, because AirDrop over AWDL is not a
 * protocol you can run on ordinary Wi-Fi: the link layer underneath takes the radio
 * exclusively and cannot coexist with WiFi.begin(). If that is a problem for your
 * project, it is a problem you should discover here rather than three days in.
 *
 * Receiving is ad_begin(), ad_on_file() and ad_poll(). See examples/PrintReceivedFiles
 * and examples/SerialPhotoBridge.
 *
 * The API is the C surface declared in the headers below. A C++ wrapper is planned on top
 * of it.
 */
#pragma once
#include "ESP32AWDL.h"

/* The dependency-free core: the codecs and the framing rules, each with a host suite. */
#include "airdrop/core/ad_dnssd.h"
#include "airdrop/core/ad_http.h"
#include "airdrop/core/ad_serve.h"
#include "airdrop/core/ad_status.h"

/* The ESP32 backend: TLS, the listener task, and the received-file handoff. */
#include "airdrop/port/ad_port_esp32.h"
