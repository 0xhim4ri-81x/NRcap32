#include "sniffer.h"

Sniffer* Sniffer::_instance = nullptr;

// ── Minimal radiotap header: version+pad+len+present, channel, dBm signal ──
// Gives Wireshark real channel/RSSI info without the overhead of a full
// radiotap field set. it_len is computed from sizeof(), so this stays
// correct if the struct ever grows.
#pragma pack(push, 1)
struct RadiotapHeader {
    uint8_t  version;
    uint8_t  pad;
    uint16_t len;
    uint32_t present;       // bit3 = CHANNEL, bit5 = DBM_ANTSIGNAL
    uint16_t chan_freq;
    uint16_t chan_flags;
    int8_t   dbm_signal;
};
#pragma pack(pop)

static uint16_t channelToFreqMHz(uint8_t ch) {
    if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
    if (ch == 14) return 2484;
    return 2412; // fallback, shouldn't happen given the 1-13 validation
}

// ── begin ─────────────────────────────────────────────────────────────────
void Sniffer::begin(BridgeProtocol& proto) {
    _proto    = &proto;
    _instance = this;
    _queue    = xQueueCreate(SNIFF_QUEUE_DEPTH, sizeof(CapturedFrame));

    // Initialise WiFi stack once here so startFixed()/startHop() never
    // call WiFi.mode() or WiFi.disconnect() — those block for 3-5 seconds
    // and cause START_SNIFF commands to time out on the Python side.
    WiFi.mode(WIFI_MODE_STA);
    WiFi.disconnect();

    // Ensure promiscuous mode starts OFF. If a previous session (or a
    // crash/reset mid-sniff) left it enabled, WiFi.scanNetworks() will
    // hang indefinitely later since the scan and the promiscuous
    // receiver compete for the radio.
    esp_wifi_set_promiscuous(false);
}

// ── buildRadiotap ────────────────────────────────────────────────────────
size_t Sniffer::buildRadiotap(uint8_t* out, uint8_t channel, int8_t rssi) {
    RadiotapHeader* rt = (RadiotapHeader*)out;
    rt->version    = 0;
    rt->pad        = 0;
    rt->len        = sizeof(RadiotapHeader);
    rt->present    = (1u << 3) | (1u << 5);
    rt->chan_freq  = channelToFreqMHz(channel);
    rt->chan_flags = 0x00A0;   // 2.4GHz, dynamic CCK-OFDM
    rt->dbm_signal = rssi;
    return sizeof(RadiotapHeader);
}

// ── startFixed ────────────────────────────────────────────────────────────
bool Sniffer::startFixed(uint8_t channel) {
    if (channel < 1 || channel > 13) return false;

    // WiFi stack is already in STA mode from begin() — skip WiFi.mode() and
    // WiFi.disconnect() here; they block for 3-5s and cause CMD timeouts.

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&Sniffer::promiscuousCb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    _channel = channel;
    _hopMode = false;
    _active  = true;
    _stats   = SniffStats{};
    return true;
}

// ── startHop ──────────────────────────────────────────────────────────────
bool Sniffer::startHop(uint16_t intervalMs) {
    if (!startFixed(1)) return false;
    _hopMode       = true;
    _hopIntervalMs = intervalMs;
    _lastHopMs     = millis();
    return true;
}

// ── stop ──────────────────────────────────────────────────────────────────
void Sniffer::stop() {
    esp_wifi_set_promiscuous(false);
    _active  = false;
    _hopMode = false;
}

// ── setChannel ────────────────────────────────────────────────────────────
bool Sniffer::setChannel(uint8_t channel) {
    if (channel < 1 || channel > 13) return false;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    _channel = channel;
    return true;
}

// ── handleHop — call from loop() ─────────────────────────────────────────
void Sniffer::handleHop() {
    if (!_active || !_hopMode) return;
    if (millis() - _lastHopMs < _hopIntervalMs) return;
    _lastHopMs = millis();
    _channel = (_channel % 13) + 1;
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
}

// ── setEapolFilter ────────────────────────────────────────────────────────
void Sniffer::setEapolFilter(bool enable, const uint8_t bssid[6]) {
    _eapolOnly      = enable;
    _hasTargetBssid = (bssid != nullptr);
    if (_hasTargetBssid) memcpy(_targetBssid, bssid, 6);
}

// ── isEapolFrame — passive check, no transmission ────────────────────────
// EAPOL (the 4-way handshake / group-key handshake) rides inside a Data
// frame as LLC/SNAP + EtherType 0x888E. We just look for that pattern at
// the right offset (accounting for the QoS control field on QoS frames).
bool Sniffer::isEapolFrame(const wifi_promiscuous_pkt_t* pkt) const {
    uint16_t len = pkt->rx_ctrl.sig_len;
    const uint8_t* p = pkt->payload;
    if (len < 26) return false;

    uint8_t fc0     = p[0];
    uint8_t ftype   = (fc0 >> 2) & 0x3;   // 2 = Data
    uint8_t subtype = (fc0 >> 4) & 0xF;
    if (ftype != 2) return false;          // only Data frames carry EAPOL

    size_t offset = 24;                    // base 802.11 MAC header
    if (subtype & 0x08) offset += 2;       // QoS control field present

    if (len < offset + 8 + 4) return false;

    // LLC/SNAP: AA AA 03 00 00 00, EtherType 88 8E
    return p[offset]   == 0xAA && p[offset+1] == 0xAA && p[offset+2] == 0x03 &&
           p[offset+3] == 0x00 && p[offset+4] == 0x00 && p[offset+5] == 0x00 &&
           p[offset+6] == 0x88 && p[offset+7] == 0x8E;
}

bool Sniffer::matchesTargetBssid(const wifi_promiscuous_pkt_t* pkt) const {
    const uint8_t* p = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 22) return false;
    const uint8_t* addr1 = p + 4;   // RA
    const uint8_t* addr2 = p + 10;  // TA
    const uint8_t* addr3 = p + 16;  // BSSID (typical for infrastructure frames)
    return memcmp(addr1, _targetBssid, 6) == 0 ||
           memcmp(addr2, _targetBssid, 6) == 0 ||
           memcmp(addr3, _targetBssid, 6) == 0;
}


void Sniffer::promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (_instance) _instance->handlePacket((wifi_promiscuous_pkt_t*)buf);
}

void Sniffer::handlePacket(wifi_promiscuous_pkt_t* pkt) {
    if (_eapolOnly) {
        if (!isEapolFrame(pkt)) return;
        if (_hasTargetBssid && !matchesTargetBssid(pkt)) return;
    }

    _stats.captured++;

    CapturedFrame f;
    uint16_t onAirLen = pkt->rx_ctrl.sig_len;
    f.orig_len = onAirLen;
    f.cap_len  = (onAirLen < SNIFF_SNAPLEN) ? onAirLen : SNIFF_SNAPLEN;
    f.channel  = pkt->rx_ctrl.channel;
    f.rssi     = pkt->rx_ctrl.rssi;
    memcpy(f.data, pkt->payload, f.cap_len);

    // Non-blocking — never stall the WiFi task. Drop on backpressure.
    if (xQueueSend(_queue, &f, 0) != pdTRUE) {
        _stats.dropped++;
    }
}

// ── processQueue — call from loop() ──────────────────────────────────────
void Sniffer::processQueue() {
    if (!_proto) return;

    CapturedFrame f;
    while (_inFlight < SNIFF_MAX_INFLIGHT && xQueueReceive(_queue, &f, 0) == pdTRUE) {
        uint8_t out[sizeof(RadiotapHeader) + SNIFF_SNAPLEN];
        size_t  rtLen = buildRadiotap(out, f.channel, f.rssi);
        memcpy(out + rtLen, f.data, f.cap_len);

        _proto->sendPcapChunk(_chunkCtr++, out, rtLen + f.cap_len);
        _inFlight++;
        _stats.sent++;
    }
    // If _inFlight is maxed, captured frames simply pile up in _queue
    // (and eventually get dropped there) until ACKs free up credit.
}

// ── onAck — wire to BridgeProtocol::onAck ───────────────────────────────────
void Sniffer::onAck(uint32_t chunkIndex) {
    (void)chunkIndex;
    if (_inFlight > 0) _inFlight--;
}