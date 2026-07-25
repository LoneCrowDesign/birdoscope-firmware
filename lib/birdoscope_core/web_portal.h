// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Untethered "Admin" web portal: a WiFi SoftAP and web UI so a phone can pull
// captured sessions and logs off the device without a laptop or serial cable.
// This is a thin adapter over the WebConsole library, declared as a lib_deps
// git dependency in platformio.ini. The implementation lives in
// web_portal.cpp, and the six functions below are the stable interface the
// board main files drive.
//
// Scope is deliberately lean. It serves stored data for download, meaning the
// SPIFFS session on every board plus the SD-card CSV logs on SD boards, and a
// `status` command. Analysis such as maps, OUI names, and filtering is out of
// scope for the firmware and consumes these downloads separately.
//
// SoftAP mode competes for the same radio as the promiscuous-mode sniffer, so
// entering the portal pauses detection for as long as it is open. The radio
// handoff, dropping promiscuous before the AP comes up and re-arming it on
// stop, is wired into the core and board code around these calls.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// SoftAP identity the boards pass to webPortalStart(). board_config.h may
// override either (it is included before this header in the board main files).
#ifndef WEB_PORTAL_AP_SSID
#define WEB_PORTAL_AP_SSID      "Birdoscope-Setup"
#endif
#ifndef WEB_PORTAL_AP_PASSWORD
#define WEB_PORTAL_AP_PASSWORD  nullptr        // open AP by default
#endif

// Starts the AP and the HTTP server. The AP is open if apPassword is NULL or
// empty, and WPA2 otherwise, which the ESP32 requires 8 or more characters
// for. No-op if already active.
void webPortalStart(const char* apSsid, const char* apPassword);

// Tears down the HTTP server and AP, returns WiFi to station mode. No-op
// if not active.
void webPortalStop();

// Services pending HTTP requests. Call every loop() iteration
// unconditionally, since it is a no-op unless webPortalActive().
void webPortalTick();

bool webPortalActive();

// Valid only while active. Backs the on-screen "browse to ___" prompt.
IPAddress webPortalIp();

// Short status line for the on-screen prompt (the "browse to http://10.99.7.1/"
// hint).
const char* webPortalStatus();
