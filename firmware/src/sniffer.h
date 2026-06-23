#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "BridgeProtocol.h"

// ── Tunables ───────────────────────────────────────────────────────────────
// Keep these conservative — ESP32-C3 has limited SRAM and the JSON/USB
// stack already eats into it. Check STATUS's "heap" field if you bump these.
#define SNIFF_SNAPLEN      400   // max 802.11 frame bytes kept per packet
#define SNIFF_QUEUE_DEPTH  24    // captured-but-unsent frames buffered
#define SNIFF_MAX_INFLIGHT 4     // un-ACKed PCAP frames allowed on the wire

// ── Captured frame (fixed size — safe to pass through a FreeRTOS queue) ────
struct CapturedFrame {
    uint16_t cap_len;            // bytes actually captured (<= SNIFF_SNAPLEN)
    uint16_t orig_len;           // true on-air frame length, for stats
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  data[SNIFF_SNAPLEN];
};

struct SniffStats {
    uint32_t captured = 0;       // frames seen by the promiscuous callback
    uint32_t sent      = 0;      // frames forwarded to Termux
    uint32_t dropped   = 0;      // frames dropped (queue full / backpressure)
};

class Sniffer {
public:
    void begin(BridgeProtocol& proto);

    // Commands (called from BridgeProtocol CMD handlers)
    bool startFixed(uint8_t channel);          // channel 1-13
    bool startHop(uint16_t intervalMs);        // cycles 1-13
    void stop();
    bool setChannel(uint8_t channel);          // valid while active, any mode

    // Passive handshake capture: when enabled, only EAPOL frames are queued
    // (optionally restricted to one BSSID). This is purely a receive-side
    // filter — it does not transmit anything. Call before start*().
    // To actually see a handshake you still need the client to (re)associate
    // naturally — e.g. toggle WiFi off/on on your own test device.
    void setEapolFilter(bool enable, const uint8_t bssid[6] = nullptr);

    // Call every loop() iteration — never blocks
    void processQueue();
    void handleHop();

    // Wire this to BridgeProtocol::onAck
    void onAck(uint32_t chunkIndex);

    bool       active()  const { return _active; }
    uint8_t    channel() const { return _channel; }
    bool       hopping() const { return _hopMode; }
    SniffStats stats()   const { return _stats; }

private:
    BridgeProtocol* _proto = nullptr;
    QueueHandle_t   _queue = nullptr;

    bool     _active       = false;
    bool     _hopMode      = false;
    uint8_t  _channel      = 1;
    uint16_t _hopIntervalMs = 300;
    uint32_t _lastHopMs    = 0;

    volatile int _inFlight = 0;
    uint8_t      _chunkCtr = 0;

    bool    _eapolOnly      = false;
    bool    _hasTargetBssid = false;
    uint8_t _targetBssid[6] = {0};

    SniffStats _stats;

    // The IDF promiscuous callback is a free function — route via singleton.
    static Sniffer* _instance;
    static void promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type);
    void   handlePacket(wifi_promiscuous_pkt_t* pkt);
    size_t buildRadiotap(uint8_t* out, uint8_t channel, int8_t rssi);
    bool   isEapolFrame(const wifi_promiscuous_pkt_t* pkt) const;
    bool   matchesTargetBssid(const wifi_promiscuous_pkt_t* pkt) const;
};