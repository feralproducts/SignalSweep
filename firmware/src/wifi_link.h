// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 B0z0dcl0wn and the SignalSweep contributors

#ifndef WIFI_LINK_H
#define WIFI_LINK_H

// ---------------------------------------------------------------------------
// Wi-Fi link: the board as its own hotspot, serving the app over HTTPS and the
// telemetry over a secure WebSocket. This is the path for devices whose
// browsers have no Web Bluetooth and no Web Serial -- every iPhone and iPad
// browser (all of them are Safari's engine underneath), Safari on a Mac, and
// Firefox anywhere. Bluetooth and USB are untouched; this is a third
// transport beside them.
//
// Why HTTPS and not plain HTTP: browsers only give location (pinning) and
// WebCrypto (the encrypted pin store) to a "secure context". The board makes
// its own certificate authority on first use; each phone installs that CA
// once from http://192.168.4.1/ and from then on https://192.168.4.1/ is a
// fully trusted, secure page.
//
// The cost, stated where the code is: one Wi-Fi radio cannot both hop
// channels looking for threats and hold a hotspot up. While the hotspot is on,
// Wi-Fi detection stays on the hotspot's channel (BLE detection is
// unaffected). That is why it is OFF by default and toggled by a BOOT
// double-tap or from the app, and why it turns itself off after
// WIFI_LINK_IDLE_OFF_MS with nobody connected.
//
// Compiled only where SIGNALSWEEP_WIFI_LINK is defined (the C5 env). Other
// boards get the inline no-op stubs below, so callers need no #ifs.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <ArduinoJson.h>

#ifdef SIGNALSWEEP_WIFI_LINK

/** @brief Read stored settings. Call once from setup(), after NVS init. Never starts the hotspot. */
void wifiLinkInit();

/** @brief Ask for the hotspot on/off. Safe from any task: the work happens in wifiLinkTick(). */
void wifiLinkRequest(bool on);

/** @brief Flip the hotspot (BOOT double-tap). Safe from any task. */
void wifiLinkToggle();

/** @brief Store a new WPA2 password (8-63 chars). Restarts the hotspot if it is up. */
bool wifiLinkSetPassword(const String& pass);

/** @brief True while the hotspot is up. */
bool wifiLinkActive();

/** @brief The channel the Wi-Fi hopper must stay on, or 0 when it is free to hop. */
int wifiLinkParkChannel();

/** @brief Queue one line (no trailing newline) for every WebSocket client. Never blocks; drops when full. */
void wifiLinkSend(const String& line);

/** @brief Drain commands from WebSocket clients, apply on/off requests, idle timeout. Call from loop(). */
void wifiLinkTick();

/** @brief Adds the hotspot's state to the config reply ("wifi", "wifi_ssid", ...). */
void wifiLinkConfigJson(JsonDocument& doc);

#else  // !SIGNALSWEEP_WIFI_LINK

inline void wifiLinkInit() {}
inline void wifiLinkRequest(bool) {}
inline void wifiLinkToggle() {}
inline bool wifiLinkSetPassword(const String&) { return false; }
inline bool wifiLinkActive() { return false; }
inline int  wifiLinkParkChannel() { return 0; }
inline void wifiLinkSend(const String&) {}
inline void wifiLinkTick() {}
inline void wifiLinkConfigJson(JsonDocument&) {}

#endif // SIGNALSWEEP_WIFI_LINK

#endif // WIFI_LINK_H
