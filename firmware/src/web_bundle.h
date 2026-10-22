// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 B0z0dcl0wn and the SignalSweep contributors

#ifndef WEB_BUNDLE_H
#define WEB_BUNDLE_H

// The app, compiled into the firmware so the Wi-Fi link can serve it with no
// filesystem image to flash (LittleFS holds the alert log and must not be
// overwritten). web_bundle.gen.cpp is written at build time by
// firmware/tools/gen_web_bundle.py from app/dist; with no app build present it
// holds a one-page placeholder that says so.

#include <stdint.h>
#include <stddef.h>

struct WebAsset {
    const char*    path;   // "/index.html", "/assets/index-abc123.js", ...
    const char*    type;   // Content-Type
    const uint8_t* data;
    uint32_t       len;
    bool           gz;     // data is gzip: send Content-Encoding: gzip
};

extern const WebAsset WEB_ASSETS[];
extern const size_t   WEB_ASSET_COUNT;

#endif // WEB_BUNDLE_H
