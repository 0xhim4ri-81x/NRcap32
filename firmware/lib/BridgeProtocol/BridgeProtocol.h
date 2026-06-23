#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ── Frame constants ───────────────────────────────────────────────────────────
#define PROTO_MAGIC_0   0xAD
#define PROTO_MAGIC_1   0xDE
#define PROTO_HEADER_SZ 8       // 2 magic + 1 type + 1 id + 4 length
#define PROTO_MAX_CHUNK 512

#define TYPE_CMD    0x01
#define TYPE_RESP   0x02
#define TYPE_EVENT  0x03
#define TYPE_PCAP   0x04
#define TYPE_ACK    0x05

// ── Parsed frame ─────────────────────────────────────────────────────────────
struct ProtoFrame {
    uint8_t  type;
    uint8_t  id;
    uint32_t length;
    uint8_t* payload;   // points into internal buffer — copy if you need it later
    bool     valid;
};

// ── Callbacks ─────────────────────────────────────────────────────────────────
using CmdCallback   = void (*)(uint8_t id, JsonDocument& doc);
using AckCallback   = void (*)(uint32_t chunk_index);

// ── BridgeProtocol ────────────────────────────────────────────────────────────
class BridgeProtocol {
public:
    BridgeProtocol(Stream& stream) : _stream(stream) {}

    // Register handlers
    void onCmd(CmdCallback cb)  { _onCmd = cb; }
    void onAck(AckCallback cb)  { _onAck = cb; }

    // Call from loop() — processes all available incoming bytes
    void update();

    // Send helpers
    void sendResp(uint8_t id, bool ok, const char* msg = nullptr);
    void sendEvent(const char* type, JsonDocument& doc);
    void sendPcapChunk(uint8_t chunk_idx, const uint8_t* data, uint32_t len);
    void sendRaw(uint8_t type, uint8_t id, const uint8_t* payload, uint32_t len);

private:
    Stream&     _stream;
    CmdCallback _onCmd = nullptr;
    AckCallback _onAck = nullptr;

    // Parser state
    uint8_t  _buf[PROTO_MAX_CHUNK + PROTO_HEADER_SZ];
    uint32_t _bufLen  = 0;

    bool     _tryParse();
    void     _dispatch(ProtoFrame& f);
    void     _resync();
};