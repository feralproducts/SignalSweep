// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 B0z0dcl0wn and the SignalSweep contributors
//
// Wi-Fi link: hotspot + HTTPS app server + secure WebSocket telemetry. See
// wifi_link.h for why it exists and what it costs.

#include "wifi_link.h"

#ifdef SIGNALSWEEP_WIFI_LINK

#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_https_server.h>
#include <freertos/ringbuf.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <mbedtls/asn1.h>
#include <mbedtls/pem.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <esp_system.h>

#include "ble_serial.h"        // processIncomingCommand()
#include "hardware_manager.h"  // chirps + LED flashes
#include "web_bundle.h"

// ---- Settings --------------------------------------------------------------
#define WIFI_NVS_NS          "sweep-wifi"
#define WIFI_DEFAULT_PASS    "signalsweep"   // documented; change it from the app
// Channel 6: the 2.4 GHz channel Wi-Fi Aware (NAN) Remote ID uses, so the one
// channel we keep watching while the hotspot is up still hears NAN drones.
#define WIFI_LINK_CHANNEL    6
#define WIFI_LINK_MAX_STA    4
#define WIFI_LINK_MAX_WS     3
// Nobody joined for this long -> hotspot off, full channel hopping back.
#define WIFI_LINK_IDLE_OFF_MS (10UL * 60UL * 1000UL)
// Outgoing line buffer. A telemetry push is ~4 KB at 1 Hz and a bulk reply
// paces ~1 KB lines every 5 ms, so 48 KB rides out a slow phone for a few
// seconds before lines are dropped (and counted). In PSRAM.
#define WIFI_TX_RING_BYTES   (48 * 1024)
#define WIFI_TX_BATCH_BYTES  (8 * 1024)
#define WIFI_RX_MAX_FRAME    (32 * 1024)     // a full rule-set push is ~11 KB
#define WIFI_RX_MAX_PENDING  (64 * 1024)

static const char* TAG = "WifiLink";
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

// ---- State -----------------------------------------------------------------
static volatile bool linkUp = false;
// Parks the hopper. Set BEFORE the AP comes up and cleared only after it is
// down: if the hopper moved the radio mid-bring-up (cert generation takes a
// second or two), the hotspot would start on whatever channel it left.
static volatile bool parkForAp = false;
static volatile int  pendingReq = -1;      // -1 none, 0 off, 1 on
static volatile bool pendingRestart = false;
static String apSsid;
static String apPass;
static uint32_t idleSinceMs = 0;

static httpd_handle_t httpsServer = NULL;
static httpd_handle_t httpServer  = NULL;

static SemaphoreHandle_t clientMutex = NULL;
// Held while the tx task is inside an httpd send, and by stopServers(): the
// server handle must not be freed under a send in flight.
static SemaphoreHandle_t sendMutex = NULL;
static int wsClients[WIFI_LINK_MAX_WS] = {-1, -1, -1};
static volatile int wsClientCount = 0;

static RingbufHandle_t txRing = NULL;
static TaskHandle_t txTask = NULL;
static volatile uint32_t txDropped = 0;

static SemaphoreHandle_t rxMutex = NULL;
static String rxPending;

// The certificate chain the HTTPS server presents, and the CA the phone
// installs. PEM, null-terminated (mbedtls wants the terminator counted).
static char* chainPem = NULL;   // server cert + CA cert
static char* keyPem   = NULL;   // server private key
static uint8_t* caDer = NULL;
static size_t caDerLen = 0;

// ============================================================================
//  TLS memory
// ============================================================================
// This core is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: every TLS
// allocation comes from the chip's internal RAM. Each HTTPS connection needs
// ~35 KB of it (16 KB in + 16 KB out record buffers, plus handshake state),
// and a phone browser opens several connections at once to load a page. With
// BLE, Wi-Fi and the detector also living in internal RAM, the handshakes ran
// out of memory and the phone saw "the network connection was lost" (first
// field test, iPhone 15, 2026-10-22). The board has 8 MB of PSRAM: send TLS
// there, falling back to internal RAM only if PSRAM is somehow full.
//
// SCOPED to the HTTP server tasks (named "httpd"), where the TLS sessions
// live. The first version swapped the allocator for every mbedtls user on the
// board -- and the Wi-Fi stack's own WPA2 handshake runs on mbedtls too; phones
// then saw the hotspot but could no longer join it (field test, 2026-10-22).
// Every other task gets exactly the core's default: internal RAM.
static volatile uint32_t tlsPsramFallbacks = 0;
static inline bool onHttpdTask() {
    const char* n = pcTaskGetName(NULL);
    return n && n[0] == 'h' && strcmp(n, "httpd") == 0;
}
static void* tlsCalloc(size_t n, size_t size) {
    if (!onHttpdTask()) return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) { p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); tlsPsramFallbacks = tlsPsramFallbacks + 1; }
    return p;
}
static void tlsFree(void* p) { heap_caps_free(p); }
static bool tlsAllocatorSet = false;
static void useTlsPsram() {
    if (tlsAllocatorSet) return;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return;   // no PSRAM: leave the default
    mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    tlsAllocatorSet = true;
}

// Counters for the /status page.
static volatile uint32_t wsOpens = 0, httpsRequests = 0;
static uint32_t linkStartedMs = 0;

// ============================================================================
//  Certificates
// ============================================================================
// One CA per board, made on first use and kept in NVS: it is what each phone
// trusts, so it must survive reboots and reflashes (a factory reset wipes it,
// and phones then need the new one). The server certificate is re-issued from
// it once per boot (the first time the hotspot comes up), dated from the firmware build so the
// 825-day ceiling iOS puts on server certificates never bites a board that
// gets updated. ECDSA P-256 throughout: fast to generate on the chip and
// accepted by iOS, Android and every desktop browser.

static int rngFn(void*, unsigned char* out, size_t len) {
    // Hardware RNG. With the Wi-Fi radio running (it is, by the time this is
    // called) esp_fill_random is a true random source.
    esp_fill_random(out, len);
    return 0;
}

static bool genKey(mbedtls_pk_context* pk) {
    mbedtls_pk_init(pk);
    if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
    return mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rngFn, NULL) == 0;
}

// "Oct 22 2026" (__DATE__) -> "20261020000000", backed off two days so a phone
// whose clock is a little behind still sees a valid certificate.
static void buildDateNotBefore(char out[24], int addYears, int addDays) {
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    const char* p = strstr(months, mon);
    int month = p ? (int)((p - months) / 3) + 1 : 1;
    struct tm t = {};
    t.tm_year = year - 1900 + addYears;
    t.tm_mon  = month - 1;
    t.tm_mday = day - 2 + addDays;
    t.tm_hour = 0;
    mktime(&t);   // normalises the day overflow/underflow
    snprintf(out, 24, "%04d%02d%02d000000", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// subjectAltName, hand-encoded: IP 192.168.4.1 and DNS signalsweep.local.
// iOS ignores the CN and requires the SAN; encoding it ourselves keeps this
// independent of which SAN types a given mbedtls writer supports.
static size_t buildSanDer(uint8_t* buf, size_t cap) {
    static const char dns[] = "signalsweep.local";
    const size_t dnsLen = sizeof(dns) - 1;
    const size_t body = (2 + 4) + (2 + dnsLen);
    if (cap < body + 2) return 0;
    size_t i = 0;
    buf[i++] = 0x30; buf[i++] = (uint8_t)body;        // SEQUENCE
    buf[i++] = 0x87; buf[i++] = 4;                    // [7] iPAddress
    memcpy(buf + i, AP_IP, 4); i += 4;
    buf[i++] = 0x82; buf[i++] = (uint8_t)dnsLen;      // [2] dNSName
    memcpy(buf + i, dns, dnsLen); i += dnsLen;
    return i;
}

// Writes one certificate. issuerKey signs; subjectKey is certified.
static bool writeCert(mbedtls_pk_context* subjectKey, mbedtls_pk_context* issuerKey,
                      const char* subjectName, const char* issuerName, bool isCa,
                      const char* notBefore, const char* notAfter,
                      unsigned char* out, size_t outLen) {
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    bool ok = false;
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] &= 0x7F;   // positive INTEGER
    if (serial[0] == 0) serial[0] = 0x01;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, subjectKey);
    mbedtls_x509write_crt_set_issuer_key(&crt, issuerKey);
    do {
        if (mbedtls_x509write_crt_set_subject_name(&crt, subjectName) != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, issuerName) != 0) break;
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0) break;
        if (mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter) != 0) break;
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, isCa ? 1 : 0, isCa ? 0 : -1) != 0) break;
#if defined(MBEDTLS_MD_CAN_SHA1)
        mbedtls_x509write_crt_set_subject_key_identifier(&crt);
        if (!isCa) mbedtls_x509write_crt_set_authority_key_identifier(&crt);
#endif
        if (isCa) {
            if (mbedtls_x509write_crt_set_key_usage(&crt,
                    MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN) != 0) break;
        } else {
            if (mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0) break;
            mbedtls_asn1_sequence eku;
            memset(&eku, 0, sizeof(eku));
            eku.buf.tag = MBEDTLS_ASN1_OID;
            eku.buf.p   = (unsigned char*)MBEDTLS_OID_SERVER_AUTH;
            eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
            eku.next    = NULL;
            if (mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku) != 0) break;
            uint8_t san[64];
            size_t sanLen = buildSanDer(san, sizeof(san));
            if (sanLen == 0) break;
            if (mbedtls_x509write_crt_set_extension(&crt, MBEDTLS_OID_SUBJECT_ALT_NAME,
                    MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME), 0, san, sanLen) != 0) break;
        }
        if (mbedtls_x509write_crt_pem(&crt, out, outLen, rngFn, NULL) != 0) break;
        ok = true;
    } while (0);
    mbedtls_x509write_crt_free(&crt);
    return ok;
}

// PEM certificate -> DER, for the /ca.crt download (iOS installs DER served
// as application/x-x509-ca-cert as a profile).
static bool pemToDer(const char* pem, uint8_t** der, size_t* derLen) {
    mbedtls_x509_crt c;
    mbedtls_x509_crt_init(&c);
    bool ok = false;
    if (mbedtls_x509_crt_parse(&c, (const unsigned char*)pem, strlen(pem) + 1) == 0) {
        *der = (uint8_t*)malloc(c.raw.len);
        if (*der) { memcpy(*der, c.raw.p, c.raw.len); *derLen = c.raw.len; ok = true; }
    }
    mbedtls_x509_crt_free(&c);
    return ok;
}

static String shortId() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char id[5];
    snprintf(id, sizeof(id), "%02X%02X", mac[4], mac[5]);
    return String(id);
}

static bool ensureCertificates() {
    if (chainPem && keyPem && caDer) return true;
    const size_t PEM_CAP = 2048;
    unsigned char* buf = (unsigned char*)malloc(PEM_CAP);
    if (!buf) return false;

    mbedtls_pk_context caKey, srvKey;
    mbedtls_pk_init(&caKey);
    mbedtls_pk_init(&srvKey);
    String caCertPem, caKeyPem;
    String caName = "CN=SignalSweep " + shortId() + " Local CA,O=SignalSweep";
    bool ok = false;

    do {
        // ---- CA: load, or make once ----
        Preferences prefs;
        prefs.begin(WIFI_NVS_NS, true);
        caKeyPem  = prefs.getString("ca_key", "");
        caCertPem = prefs.getString("ca_crt", "");
        prefs.end();
        bool haveCa = caKeyPem.length() > 0 && caCertPem.length() > 0 &&
            mbedtls_pk_parse_key(&caKey, (const unsigned char*)caKeyPem.c_str(), caKeyPem.length() + 1,
                                 NULL, 0, rngFn, NULL) == 0;
        if (!haveCa) {
            mbedtls_pk_free(&caKey);
            if (!genKey(&caKey)) { ESP_LOGE(TAG, "CA keygen failed"); break; }
            if (mbedtls_pk_write_key_pem(&caKey, buf, PEM_CAP) != 0) break;
            caKeyPem = String((const char*)buf);
            // A CA is not subject to the 825-day server-certificate rule; 20
            // years so a phone set up once never has to be set up again.
            char nb[24], na[24];
            buildDateNotBefore(nb, 0, 0);
            buildDateNotBefore(na, 20, 0);
            if (!writeCert(&caKey, &caKey, caName.c_str(), caName.c_str(), true, nb, na, buf, PEM_CAP)) {
                ESP_LOGE(TAG, "CA cert write failed"); break;
            }
            caCertPem = String((const char*)buf);
            prefs.begin(WIFI_NVS_NS, false);
            prefs.putString("ca_key", caKeyPem);
            prefs.putString("ca_crt", caCertPem);
            prefs.end();
            if (Serial) Serial.println("[WiFi] New board certificate authority created");
        }
        // Parse the stored CA cert for its subject: issuer name must match it
        // byte for byte or the chain does not verify.
        {
            mbedtls_x509_crt c;
            mbedtls_x509_crt_init(&c);
            if (mbedtls_x509_crt_parse(&c, (const unsigned char*)caCertPem.c_str(), caCertPem.length() + 1) != 0) {
                mbedtls_x509_crt_free(&c); break;
            }
            char dn[160];
            if (mbedtls_x509_dn_gets(dn, sizeof(dn), &c.subject) > 0) caName = String(dn);
            mbedtls_x509_crt_free(&c);
        }

        // ---- Server cert: fresh each boot ----
        if (!genKey(&srvKey)) { ESP_LOGE(TAG, "server keygen failed"); break; }
        if (mbedtls_pk_write_key_pem(&srvKey, buf, PEM_CAP) != 0) break;
        String srvKeyPem = String((const char*)buf);
        char nb[24], na[24];
        buildDateNotBefore(nb, 0, 0);
        buildDateNotBefore(na, 0, 820);   // under iOS's 825-day ceiling
        if (!writeCert(&srvKey, &caKey, "CN=192.168.4.1,O=SignalSweep", caName.c_str(), false, nb, na, buf, PEM_CAP)) {
            ESP_LOGE(TAG, "server cert write failed"); break;
        }
        String chain = String((const char*)buf) + caCertPem;

        free(chainPem); free(keyPem); free(caDer);
        chainPem = strdup(chain.c_str());
        keyPem   = strdup(srvKeyPem.c_str());
        caDer = NULL; caDerLen = 0;
        if (!chainPem || !keyPem || !pemToDer(caCertPem.c_str(), &caDer, &caDerLen)) break;
        ok = true;
    } while (0);

    mbedtls_pk_free(&caKey);
    mbedtls_pk_free(&srvKey);
    free(buf);
    if (!ok) {
        free(chainPem); free(keyPem); free(caDer);
        chainPem = NULL; keyPem = NULL; caDer = NULL; caDerLen = 0;
    }
    return ok;
}

// ============================================================================
//  WebSocket clients
// ============================================================================

static void addClient(int fd) {
    bool added = false;
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    for (int i = 0; i < WIFI_LINK_MAX_WS; i++) if (wsClients[i] == fd) { added = true; break; }
    if (!added) {
        for (int i = 0; i < WIFI_LINK_MAX_WS; i++) {
            if (wsClients[i] < 0) { wsClients[i] = fd; wsClientCount = wsClientCount + 1; added = true; break; }
        }
    }
    xSemaphoreGive(clientMutex);
    if (added) {
        playConnectionChirp();
        if (Serial) Serial.printf("[WiFi] app connected (fd %d)\n", fd);
    }
}

static void removeClient(int fd) {
    bool removed = false;
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    for (int i = 0; i < WIFI_LINK_MAX_WS; i++) {
        if (wsClients[i] == fd) { wsClients[i] = -1; wsClientCount = wsClientCount - 1; removed = true; }
    }
    xSemaphoreGive(clientMutex);
    if (removed) {
        playDisconnectionChirp();
        if (Serial) Serial.printf("[WiFi] app disconnected (fd %d)\n", fd);
    }
}

static void clearClients() {
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    for (int i = 0; i < WIFI_LINK_MAX_WS; i++) wsClients[i] = -1;
    wsClientCount = 0;
    xSemaphoreGive(clientMutex);
}

// One frame to every connected client. Drops any client the send fails on.
static void sendToAll(const uint8_t* data, size_t len) {
    if (len == 0) return;
    int fds[WIFI_LINK_MAX_WS]; int n = 0;
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    for (int i = 0; i < WIFI_LINK_MAX_WS; i++) if (wsClients[i] >= 0) fds[n++] = wsClients[i];
    xSemaphoreGive(clientMutex);
    int dead[WIFI_LINK_MAX_WS]; int nd = 0;
    xSemaphoreTake(sendMutex, portMAX_DELAY);
    if (linkUp && httpsServer) {
        httpd_ws_frame_t f = {};
        f.final = true; f.type = HTTPD_WS_TYPE_TEXT;
        f.payload = (uint8_t*)data; f.len = len;
        for (int i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(httpsServer, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET ||
                httpd_ws_send_data(httpsServer, fds[i], &f) != ESP_OK) dead[nd++] = fds[i];
        }
    }
    xSemaphoreGive(sendMutex);
    for (int i = 0; i < nd; i++) removeClient(dead[i]);
}

// Drains the outgoing ring to every client. A dedicated task so no caller of
// wifiLinkSend() -- the detector, the NimBLE host task, loop() -- ever waits on
// a phone's TLS socket.
static void txTaskFn(void*) {
    uint8_t* batch = (uint8_t*)heap_caps_malloc(WIFI_TX_BATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!batch) batch = (uint8_t*)malloc(WIFI_TX_BATCH_BYTES);
    for (;;) {
        size_t sz = 0;
        void* item = xRingbufferReceive(txRing, &sz, pdMS_TO_TICKS(250));
        if (!item) continue;
        if (!linkUp || !batch || sz > WIFI_TX_BATCH_BYTES) {
            // An oversized line (never expected: lines are < 4 KB) goes alone.
            if (linkUp) sendToAll((const uint8_t*)item, sz);
            vRingbufferReturnItem(txRing, item);
            continue;
        }
        // Coalesce whatever else is waiting: one TLS record carrying several
        // lines costs far less than one per line. Order is preserved.
        size_t used = sz;
        memcpy(batch, item, sz);
        vRingbufferReturnItem(txRing, item);
        for (;;) {
            size_t more = 0;
            void* next = xRingbufferReceive(txRing, &more, 0);
            if (!next) break;
            if (used + more > WIFI_TX_BATCH_BYTES) {
                sendToAll(batch, used);
                used = 0;
                if (more > WIFI_TX_BATCH_BYTES) {
                    sendToAll((const uint8_t*)next, more);
                    vRingbufferReturnItem(txRing, next);
                    continue;
                }
            }
            memcpy(batch + used, next, more);
            used += more;
            vRingbufferReturnItem(txRing, next);
        }
        sendToAll(batch, used);
    }
}

void wifiLinkSend(const String& line) {
    if (!linkUp || wsClientCount <= 0 || !txRing) return;
    size_t len = line.length();
    if (len == 0) return;
    // Each item carries its own newline: the app reassembles on '\n' exactly
    // as it does for BLE notifications and the serial stream.
    void* slot = NULL;
    if (xRingbufferSendAcquire(txRing, &slot, len + 1, 0) != pdTRUE || !slot) { txDropped = txDropped + 1; return; }
    memcpy(slot, line.c_str(), len);
    ((char*)slot)[len] = '\n';
    xRingbufferSendComplete(txRing, slot);
}

// ============================================================================
//  HTTP handlers
// ============================================================================

static esp_err_t wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {   // the upgrade handshake
        wsOpens = wsOpens + 1;
        addClient(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t f = {};
    esp_err_t r = httpd_ws_recv_frame(req, &f, 0);   // length only
    if (r != ESP_OK) return r;
    if (f.len == 0) return ESP_OK;
    if (f.len > WIFI_RX_MAX_FRAME) return ESP_FAIL;  // closes the socket
    uint8_t* buf = (uint8_t*)malloc(f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    r = httpd_ws_recv_frame(req, &f, f.len);
    if (r == ESP_OK && f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len] = 0;
        xSemaphoreTake(rxMutex, portMAX_DELAY);
        if (rxPending.length() + f.len < WIFI_RX_MAX_PENDING) rxPending.concat((const char*)buf, f.len);
        xSemaphoreGive(rxMutex);
    }
    free(buf);
    return r;
}

static const WebAsset* findAsset(const char* uri) {
    char path[160];
    size_t n = strcspn(uri, "?#");
    if (n >= sizeof(path)) n = sizeof(path) - 1;
    memcpy(path, uri, n); path[n] = 0;
    if (strcmp(path, "/") == 0) strcpy(path, "/index.html");
    for (size_t i = 0; i < WEB_ASSET_COUNT; i++) {
        if (strcmp(WEB_ASSETS[i].path, path) == 0) return &WEB_ASSETS[i];
    }
    return NULL;
}

static esp_err_t sendCaCert(httpd_req_t* req) {
    if (!caDer) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no certificate");
    httpd_resp_set_type(req, "application/x-x509-ca-cert");
    String disp = "attachment; filename=\"SignalSweep-" + shortId() + "-CA.crt\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    return httpd_resp_send(req, (const char*)caDer, caDerLen);
}

static esp_err_t staticHandler(httpd_req_t* req) {
    httpsRequests = httpsRequests + 1;
    if (strncmp(req->uri, "/ca.crt", 7) == 0) return sendCaCert(req);
    const WebAsset* a = findAsset(req->uri);
    if (!a) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    httpd_resp_set_type(req, a->type);
    if (a->gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // Hashed asset names never change content; index.html and app.js do.
    const bool hashed = strncmp(a->path, "/assets/", 8) == 0;
    httpd_resp_set_hdr(req, "Cache-Control", hashed ? "public, max-age=31536000, immutable" : "no-cache");
    // Send in slices: the TLS layer copies each into a record anyway, and one
    // 300 KB send holds the server task far longer than it needs to.
    const size_t SLICE = 4096;
    for (uint32_t off = 0; off < a->len; off += SLICE) {
        size_t n = a->len - off < SLICE ? a->len - off : SLICE;
        if (httpd_resp_send_chunk(req, (const char*)a->data + off, n) != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Plain-HTTP setup page on port 80. Its only jobs: hand out the CA, and point
// at the secure app. Everything else lives on https.
static const char SETUP_PAGE[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>SignalSweep setup</title><style>"
"body{font:16px -apple-system,system-ui,sans-serif;background:#0a0e16;color:#e6edf3;margin:0;padding:20px;max-width:560px}"
"h1{font-size:22px}a.b{display:block;text-align:center;padding:14px;margin:14px 0;border-radius:10px;"
"background:#1f6feb;color:#fff;text-decoration:none;font-weight:600}a.b.s{background:#2d333b}"
"ol{padding-left:20px;line-height:1.5}small{color:#8b949e}</style></head><body>"
"<h1>SignalSweep</h1>"
"<a class=b href='https://192.168.4.1/'>Open SignalSweep</a>"
"<p><b>First time on this phone?</b> Install this board's certificate once, or the page above "
"will show a security warning and location pins won't work.</p>"
"<a class='b s' href='/ca.crt'>1. Download certificate</a>"
"<ol><li><b>iPhone/iPad:</b> tap Allow. Open <b>Settings</b> &rarr; <b>Profile Downloaded</b> &rarr; "
"<b>Install</b>. Then <b>Settings &rarr; General &rarr; About &rarr; Certificate Trust Settings</b> and "
"turn on full trust for <i>SignalSweep Local CA</i>.</li>"
"<li><b>Android:</b> Settings &rarr; Security &rarr; Encryption &amp; credentials &rarr; Install a certificate "
"&rarr; CA certificate, and pick the downloaded file.</li>"
"<li><b>Mac/PC:</b> open the file and mark it trusted (Keychain Access on a Mac).</li></ol>"
"<small>Each board has its own certificate. A factory reset makes a new one. "
"Trouble? <a href='/status' style='color:#8b949e'>Board status</a></small>"
"</body></html>";

// Plain-text health readout at http://192.168.4.1/status -- readable from any
// phone on the hotspot, so a field problem can be diagnosed without a laptop.
static esp_err_t sendStatus(httpd_req_t* req) {
    char b[700];
    int n = snprintf(b, sizeof(b),
        "SignalSweep hotspot status\n"
        "ssid: %s  channel: %d\n"
        "hotspot up for: %lu s   board up for: %lu s   last reset reason: %d\n"
        "phones joined: %d   app connections (websocket): %d   websocket opens: %lu\n"
        "https requests served: %lu   https server running: %s\n"
        "internal RAM free: %u (lowest %u, largest block %u)\n"
        "PSRAM free: %u of %u\n"
        "TLS memory in PSRAM: %s   fallbacks to internal: %lu\n"
        "lines dropped (slow phone): %lu\n",
        apSsid.c_str(), WIFI_LINK_CHANNEL,
        (unsigned long)((millis() - linkStartedMs) / 1000), (unsigned long)(millis() / 1000), (int)esp_reset_reason(),
        WiFi.softAPgetStationNum(), (int)wsClientCount, (unsigned long)wsOpens,
        (unsigned long)httpsRequests, httpsServer ? "yes" : "NO",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        tlsAllocatorSet ? "yes" : "NO", (unsigned long)tlsPsramFallbacks,
        (unsigned long)txDropped);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, b, n > 0 && n < (int)sizeof(b) ? n : (int)strlen(b));
}

static esp_err_t setupHandler(httpd_req_t* req) {
    if (strncmp(req->uri, "/status", 7) == 0) return sendStatus(req);
    if (strncmp(req->uri, "/ca.crt", 7) == 0) return sendCaCert(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, SETUP_PAGE, sizeof(SETUP_PAGE) - 1);
}

// ============================================================================
//  Bring-up / tear-down (loop() context only)
// ============================================================================

static void reassertPromiscuous(bool wasOn) {
    // A mode change can drop promiscuous reception; put the detector's back.
    if (wasOn) esp_wifi_set_promiscuous(true);
}

static void configureDhcpNoGateway() {
    // Offer an address but no router and no DNS. A phone then keeps its
    // cellular data as the default route, so the map can still load tiles
    // while it talks to 192.168.4.1 over Wi-Fi. (Needs field confirmation
    // per phone model; worst case the map is blank and pins still work.)
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return;
    esp_netif_dhcps_stop(ap);
    uint8_t none = 0;   // dhcps_offer_t: no OFFER_ROUTER, no OFFER_DNS
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none, sizeof(none));
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &none, sizeof(none));
    esp_netif_dhcps_start(ap);
}

static void stopServers() {
    if (sendMutex) xSemaphoreTake(sendMutex, portMAX_DELAY);
    if (httpsServer) { httpd_ssl_stop(httpsServer); httpsServer = NULL; }
    if (httpServer)  { httpd_stop(httpServer); httpServer = NULL; }
    if (sendMutex) xSemaphoreGive(sendMutex);
    clearClients();
}

static bool startServers() {
    httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
    sc.servercert     = (const uint8_t*)chainPem;
    sc.servercert_len = strlen(chainPem) + 1;
    sc.prvtkey_pem    = (const uint8_t*)keyPem;
    sc.prvtkey_len    = strlen(keyPem) + 1;
    // cacert_pem stays NULL: it would demand client certificates.
    // Safari opens several connections at once to load a page. With too few
    // slots the LRU purge could close the live WebSocket to make room; the
    // app also sends a keepalive every few seconds so the socket stays
    // recently used. 6 TLS + 2 plain + the servers' own = 14 of 16 lwIP sockets.
    sc.httpd.max_open_sockets = 6;
    sc.httpd.max_uri_handlers = 4;
    sc.httpd.lru_purge_enable = true;
    sc.httpd.uri_match_fn = httpd_uri_match_wildcard;
    sc.httpd.stack_size = 16384;   // TLS handshake + ECDSA on this task
    if (httpd_ssl_start(&httpsServer, &sc) != ESP_OK) { httpsServer = NULL; return false; }

    httpd_uri_t ws = {};
    ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = wsHandler;
    ws.is_websocket = true; ws.handle_ws_control_frames = false;
    httpd_register_uri_handler(httpsServer, &ws);
    httpd_uri_t any = {};
    any.uri = "/*"; any.method = HTTP_GET; any.handler = staticHandler;
    httpd_register_uri_handler(httpsServer, &any);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    hc.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 2;   // the SSL server took +1
    hc.max_uri_handlers = 2;
    hc.max_open_sockets = 2;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.lru_purge_enable = true;
    if (httpd_start(&httpServer, &hc) == ESP_OK) {
        httpd_uri_t setup = {};
        setup.uri = "/*"; setup.method = HTTP_GET; setup.handler = setupHandler;
        httpd_register_uri_handler(httpServer, &setup);
    } else {
        httpServer = NULL;   // the app still works; only the setup page is gone
    }
    return true;
}

static void linkStart() {
    if (linkUp) return;
    parkForAp = true;
    vTaskDelay(pdMS_TO_TICKS(400));   // > one hop dwell: the hopper sees the park
    bool promisc = false;
    esp_wifi_get_promiscuous(&promisc);

    WiFi.mode(WIFI_AP_STA);
    IPAddress ip(AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(apSsid.c_str(), apPass.c_str(), WIFI_LINK_CHANNEL, 0, WIFI_LINK_MAX_STA)) {
        if (Serial) Serial.println("[WiFi] hotspot failed to start");
        WiFi.mode(WIFI_STA);
        reassertPromiscuous(promisc);
        parkForAp = false;
        triggerLedFlash(255, 0, 0, 600);
        return;
    }
    // Pin the radio to the hotspot channel now, before any phone joins.
    esp_wifi_set_channel(WIFI_LINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    configureDhcpNoGateway();
    reassertPromiscuous(promisc);

    useTlsPsram();
    if (!ensureCertificates() || !startServers()) {
        if (Serial) Serial.println("[WiFi] certificate or server start failed");
        stopServers();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        reassertPromiscuous(promisc);
        parkForAp = false;
        triggerLedFlash(255, 0, 0, 600);
        return;
    }
    linkUp = true;
    idleSinceMs = millis();
    linkStartedMs = millis();
    triggerLedFlash(0, 200, 255, 800);
    playConnectionChirp();
    if (Serial) Serial.printf("[WiFi] hotspot up: SSID %s, https://192.168.4.1/ (channel %d)\n",
                              apSsid.c_str(), WIFI_LINK_CHANNEL);
}

static void linkStop() {
    if (!linkUp) return;
    bool promisc = false;
    esp_wifi_get_promiscuous(&promisc);
    linkUp = false;
    stopServers();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    reassertPromiscuous(promisc);
    parkForAp = false;   // only now may the hopper move the radio again
    if (txRing) {   // drop anything still queued for clients that are gone
        size_t sz; void* it;
        while ((it = xRingbufferReceive(txRing, &sz, 0)) != NULL) vRingbufferReturnItem(txRing, it);
    }
    triggerLedFlash(255, 140, 0, 600);
    playDisconnectionChirp();
    if (Serial) Serial.println("[WiFi] hotspot off -- full channel hopping resumed");
}

// ============================================================================
//  Public API
// ============================================================================

void wifiLinkInit() {
    if (!clientMutex) clientMutex = xSemaphoreCreateMutex();
    if (!sendMutex) sendMutex = xSemaphoreCreateMutex();
    if (!rxMutex) rxMutex = xSemaphoreCreateMutex();
    if (!txRing) {
        txRing = xRingbufferCreateWithCaps(WIFI_TX_RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!txRing) txRing = xRingbufferCreate(16 * 1024, RINGBUF_TYPE_NOSPLIT);
    }
    if (txRing && !txTask) xTaskCreate(txTaskFn, "WifiLinkTx", 4096, NULL, 3, &txTask);
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, true);
    apPass = prefs.getString("pass", WIFI_DEFAULT_PASS);
    prefs.end();
    if (apPass.length() < 8) apPass = WIFI_DEFAULT_PASS;
    apSsid = "SignalSweep-" + shortId();
}

void wifiLinkRequest(bool on) { pendingReq = on ? 1 : 0; }
void wifiLinkToggle() { pendingReq = linkUp ? 0 : 1; }
bool wifiLinkActive() { return linkUp; }
int  wifiLinkParkChannel() { return (linkUp || parkForAp) ? WIFI_LINK_CHANNEL : 0; }

bool wifiLinkSetPassword(const String& pass) {
    if (pass.length() < 8 || pass.length() > 63) return false;
    Preferences prefs;
    prefs.begin(WIFI_NVS_NS, false);
    prefs.putString("pass", pass);
    prefs.end();
    apPass = pass;
    if (linkUp) pendingRestart = true;
    return true;
}

void wifiLinkConfigJson(JsonDocument& doc) {
    doc["wifi"] = (bool)linkUp;
    doc["wifi_ssid"] = apSsid;
    doc["wifi_pass"] = apPass;
    doc["wifi_ch"] = WIFI_LINK_CHANNEL;
    doc["wifi_clients"] = (int)wsClientCount;
    doc["wifi_drop"] = (uint32_t)txDropped;
}

void wifiLinkTick() {
    int req = pendingReq;
    bool changed = false;
    if (req >= 0) {
        pendingReq = -1;
        bool was = linkUp;
        if (req == 1) linkStart(); else linkStop();
        changed = was != linkUp || req == 1;   // also answer a failed start
    }
    if (pendingRestart) {
        pendingRestart = false;
        if (linkUp) { linkStop(); linkStart(); changed = true; }
    }
    if (changed) {
        // The command that asked was answered before this ran; tell every
        // connected app what actually happened.
        String cfg = getBleConfigJson();
        sendBleSerial(cfg);
        sendUsbLine(cfg);
    }
    if (!linkUp) return;

    // Commands from WebSocket clients run here, on loop(), exactly like USB
    // commands -- never on the HTTP server task.
    String lines;
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    int last = rxPending.lastIndexOf('\n');
    if (last >= 0) {
        lines = rxPending.substring(0, last + 1);
        rxPending.remove(0, last + 1);
    }
    xSemaphoreGive(rxMutex);
    int start = 0;
    while (start < (int)lines.length()) {
        int nl = lines.indexOf('\n', start);
        if (nl < 0) break;
        String cmd = lines.substring(start, nl);
        start = nl + 1;
        cmd.trim();
        if (cmd.length() == 0 || cmd == "CMD:HOST" || cmd == "CMD:HOST:BYE") continue;
        processIncomingCommand(cmd);
    }

    // Idle shutoff: nobody on the hotspot for a while gives the detector its
    // full channel sweep back.
    if (WiFi.softAPgetStationNum() > 0 || wsClientCount > 0) {
        idleSinceMs = millis();
    } else if (millis() - idleSinceMs > WIFI_LINK_IDLE_OFF_MS) {
        if (Serial) Serial.println("[WiFi] nobody connected for 10 min -- turning hotspot off");
        linkStop();
        String cfg = getBleConfigJson();
        sendBleSerial(cfg);
        sendUsbLine(cfg);
    }
}

#endif // SIGNALSWEEP_WIFI_LINK
