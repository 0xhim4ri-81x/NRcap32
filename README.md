# NRcap32

> Turn a $3–5 ESP32 into a Wi-Fi capture adapter for Termux — **no root required.**

NRcap32 bypasses Android's locked-down Wi-Fi API entirely by offloading the radio layer to an ESP32 over USB OTG. The result: monitor mode, passive packet capture, radiotap-annotated pcap output, and EAPOL handshake capture — all on a stock, unrooted Android phone.

```
Stock Android phone  ──USB OTG──  ESP32-C3 SuperMini (~$3)
       │                                    │
  Termux (Python)       ←──────────────────→       Raw 802.11 frames
  No root. No ROM.         Binary protocol          Monitor mode
```

<p align="center">
  <img src="docs/images/hardware.jpg" width="600" alt="ESP32-C3-SuperMini connected to Android phone via OTG cable">
</p>


> ⚠️ **Authorized use only.** Only use this tool on networks and devices you own or have explicit written permission to test. Unauthorized interception of network traffic is illegal in most jurisdictions.

---

## Why this exists

Android has never exposed monitor mode or raw packet injection to user-space apps — not even with root on most devices. The traditional path required a rooted phone, a custom kernel (e.g. NetHunter), a supported external USB Wi-Fi adapter, and a lot of luck matching hardware versions.

NRcap32 sidesteps the entire problem:

- The **ESP32** handles everything radio-level — promiscuous capture, channel hopping, raw frame injection, EAPOL filtering
- **Termux** talks to it over USB CDC using `termux-usb` + libusb — no root, no kernel modules, no custom ROM
- The **Python bridge** speaks a compact framed binary protocol with flow control, not serial ASCII

Total hardware cost: under $5. Works on any Android phone with USB-C or micro-USB and an OTG cable.

---

## Features

| | |
|---|---|
| **Wi-Fi scan** | Active scan with SSID, BSSID, channel, RSSI, and security type |
| **Packet capture** | Fixed channel or channel-hopping promiscuous capture, saved as `.pcap` with radiotap headers (Wireshark-compatible) |
| **EAPOL capture** | Passive handshake capture, optionally filtered by BSSID |
| **Deauth + capture** | Send deauth frames to trigger a handshake, capture EAPOL simultaneously |
| **Live streaming** | Pipe pcap output to termshark via named FIFO for real-time analysis |
| **Heartbeat** | ESP32 sends uptime and free heap every 5 seconds for health monitoring |
| **Modular firmware** | Designed to be extended with new CMDs — IR, NRF24, and CC1101 modules planned |

---

## Hardware

| Component | Status | Notes |
|-----------|:------:|-------|
| **ESP32-C3 SuperMini** | ✅ Tested | Primary target — ~$3, native USB CDC built-in |
| ESP32-S3 (any board) | ⚠️ Untested | More RAM/flash, native USB CDC, dual-core |
| ESP32-S2 (any board) | ⚠️ Untested | Single-core, native USB CDC, lower cost |
| USB OTG cable / adapter | — | USB-C OTG or micro-USB OTG depending on your phone |
| Android phone | — | Any version with USB host support — no root required |

All supported boards use **native USB CDC** — no external USB-UART bridge chip (CP2102/CH340) needed. They show up as `303A:xxxx` on Android and communicate directly over USB.

The firmware uses the ESP32's built-in 2.4 GHz radio — no external antenna needed.

---

## Quick Start

### 1. Install dependencies (Termux)

```bash
pkg update && pkg install python termux-api libusb
pip install pyusb
```

Also install the **Termux:API** companion app from [F-Droid](https://f-droid.org) — *not* the Play Store version, which is outdated.

### 2. Clone the repo

```bash
git clone https://github.com/0xhim4ri-81x/nrcap32
cd nrcap32
```

### 3. Flash the firmware

```bash
cd firmware/

# Flash to ESP32-C3 SuperMini (primary tested target)
pio run -e esp32-c3-supermini --target upload

# Confirm it booted correctly — you should see: ESP32_READY
pio device monitor --baud 115200
```

Pre-built binaries are also available on the [Releases](https://github.com/0xhim4ri-81x/nrcap32/releases) page if you don't want to build from source. See the [Firmware section](#firmware) for `esptool.py` flashing instructions.

### 4. Connect and run

Plug the ESP32 into your phone via OTG cable. On first run, Android shows a USB permission dialog — tap **OK**. The permission persists until you unplug.

```bash
chmod +x nrcap32
./nrcap32 scan
```

**Or use the automated installer:**
```bash
curl -sSL https://raw.githubusercontent.com/0xhim4ri-81x/nrcap32/main/install.sh | bash
```

---

## Usage

All commands follow the same pattern — the script detects the USB device, requests permission via `termux-usb`, and re-invokes itself with the granted file descriptor.

### Scan nearby networks

```bash
./nrcap32 scan
```

```
Found device: /dev/bus/usb/002/008
Requesting USB permission (tap OK)...

[2026-06-19 17:38:32] === WiFi Network Scan ===
[2026-06-19 17:38:32] Device: 303A:1001  ESP32-S3 native USB CDC

==========================================================================================
SSID                           BSSID               CH   RSSI  SECURITY
------------------------------------------------------------------------------------------
HomeNetwork                    C8:3A:35:CA:75:48   11    -64  WPA/WPA2-PSK
OfficeWifi                     B0:4E:26:CC:2E:F8    3    -66  WPA2-PSK

2 networks found.
```

### Capture packets

```bash
# Fixed channel
./nrcap32 sniff --channel 6
./nrcap32 sniff --channel 6 -o capture.pcap

# Channel hopping (hops 1–13 every 300ms, stop with Ctrl+C)
./nrcap32 sniff --hop --interval 300
```

### Open in Wireshark / termshark

```bash
# Transfer the file and open on PC
wireshark capture.pcap

# Live via termshark on the phone
mkfifo /tmp/live.pcap
termshark -i /tmp/live.pcap &
./nrcap32 sniff --channel 6 -o /tmp/live.pcap
```

### Capture EAPOL handshake (passive)

```bash
./nrcap32 sniff --channel 6 --eapol-only --bssid C8:3A:35:CA:75:48
```

### Deauth + handshake capture

```bash
./nrcap32 deauth \
    --bssid C8:3A:35:CA:75:48 \
    --channel 11 \
    --count 15 \
    --capture-secs 30
```

Sends 15 deauth frames to disconnect clients, then captures EAPOL frames for 30 seconds. The resulting pcap can be used with Hashcat or Aircrack-ng for offline WPA2 key testing.

> ⚠️ Only use this on your own network or with explicit written permission from the network owner.

---

## Environments

NRcap32 supports three runtime environments, detected automatically via `detect_backend()`. The same CLI commands work identically in all three.

### No root (stock Termux)

The most unique use-case. Works on any unmodified Android phone.

```
Android USB host stack
        ↓
   termux-usb  (requests permission, grants fd)
        ↓
libusb_wrap_sys_device(fd)
        ↓
   nrcap32 Python
```

```bash
pkg install python termux-api libusb && pip install pyusb
# Install Termux:API from F-Droid
./nrcap32 scan  # permission popup appears on first run
```

### With root (Termux + tsu/sudo)

When running as uid 0, libusb opens `/dev/bus/usb` directly — no `termux-usb`, no permission dialog, no bootstrap subprocess. Faster and simpler.

```bash
pkg install python libusb && pip install pyusb
tsu  # or: sudo ./nrcap32 scan
./nrcap32 scan
```

### Kali NetHunter

Works in both the NetHunter terminal (Kali chroot, root) and a Termux session running alongside it.

```bash
# In the NetHunter terminal (root)
apt update && apt install python3 python3-pip libusb-1.0-0
pip3 install pyusb
./nrcap32 scan

# Note: if cdc_acm claimed the interface, nrcap32 detaches it automatically
```

### Backend summary

| Environment | uid | USB access |
|---|:---:|---|
| Stock Termux (no root) | 1000 | `termux-usb` → `libusb_wrap_sys_device(fd)` |
| Termux + root (tsu/sudo) | 0 | libusb → `/dev/bus/usb` directly |
| NetHunter terminal | 0 | libusb → `/dev/bus/usb` directly |
| NetHunter + Termux (no root) | 1000 | `termux-usb` → `libusb_wrap_sys_device(fd)` |

Detection is fully automatic — no `--root` flag or manual configuration needed.

---

## Comparison

| | NRcap32 (no root) | NRcap32 (root) | NetHunter + ext. adapter |
|--|:-----------------:|:--------------:|:------------------------:|
| Root required | ✗ | ✓ | ✓ |
| Custom kernel / ROM | ✗ | ✗ | ✓ |
| Hardware cost | ~$5 | ~$5 | $150–400+ |
| Supported devices | Any Android | Any Android | NetHunter-supported only |
| Monitor mode | ✓ (ESP32) | ✓ (ESP32) | ✓ |
| Packet injection | ✓ | ✓ | ✓ |
| USB permission dialog | First use only | ✗ | ✗ |
| Live Wireshark (FIFO) | ✓ | ✓ | ✓ |
| IR / RF expansion | Planned | Planned | ✗ |

---

## Firmware

### Supported boards

| Board | `platformio.ini` env | VID:PID | Status |
|-------|---------------------|:-------:|:------:|
| ESP32-C3 SuperMini | `esp32-c3-supermini` | `303A:1001` | ✅ Tested |
| ESP32-S3 | `esp32-s3` | `303A:1001` | ⚠️ Untested |
| ESP32-S2 | `esp32-s2` | `303A:0002` | ⚠️ Untested |

### Key build flags

```ini
build_flags =
    -DARDUINO_USB_MODE=1           ; route Serial to native USB, not UART pins
    -DARDUINO_USB_CDC_ON_BOOT=1    ; enable CDC before setup() runs  ← CRITICAL
    -DCONFIG_AUTOSTART_ARDUINO=1   ; loop() won't run without this
    -DCONFIG_ESP_WIFI_ENABLE_SNIFFER=1
```

> **`ARDUINO_USB_CDC_ON_BOOT=1` is critical** — without it `Serial` maps to UART0 (TX/RX pins) and the bridge protocol gets no data over USB.

### Flashing with PlatformIO

```bash
cd firmware/

# Flash + open serial monitor
pio run -e esp32-c3-supermini --target upload && pio device monitor

# Confirm boot — should print: ESP32_READY
# Heartbeat JSON appears every 5s: {"uptime":5000,"heap":250336,"type":"heartbeat"}
```

### Flashing pre-built binaries

Pre-built `.bin` files are on the [Releases](https://github.com/0xhim4ri-81x/nrcap32/releases) page.

```bash
pip install esptool

esptool.py --chip esp32c3 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-c3.bin
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-s3.bin
esptool.py --chip esp32s2 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-s2.bin
```

Or use the [ESP32 Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) on Windows.

---

## Bridge Protocol

NRcap32 uses a compact binary framing protocol over USB CDC bulk transfer.

```
[0xAD 0xDE][TYPE 1B][ID 1B][LENGTH 4B LE][PAYLOAD NB]
```

| Type | Hex | Direction | Payload |
|------|:---:|-----------|---------|
| CMD | `0x01` | Termux → ESP32 | JSON `{"cmd": "...", "args": {...}}` |
| RESP | `0x02` | ESP32 → Termux | JSON response, same ID as CMD |
| EVENT | `0x03` | ESP32 → Termux | JSON async event (scan results, heartbeat) |
| PCAP | `0x04` | ESP32 → Termux | Raw binary: radiotap header + 802.11 frame |
| ACK | `0x05` | Termux → ESP32 | JSON `{"chunk": N}` — flow control |

PCAP frames use sliding-window flow control: the ESP32 buffers up to `SNIFF_MAX_INFLIGHT` (default 4) unacknowledged frames before pausing.

### Supported CMDs

| CMD | Args | Response |
|-----|------|----------|
| `PING` | — | `{"ok": true, "msg": "pong"}` |
| `STATUS` | — | `{"ok": true, "uptime": ms, "heap": bytes, "chip": "...", "sniffing": bool}` |
| `SCAN_WIFI` | — | `{"ok": true, "count": N}` + N `scan_ap` events |
| `START_SNIFF` | `{"mode": "fixed"\|"hop", "channel": 1-13, ...}` | `{"ok": true}` |
| `STOP_SNIFF` | — | `{"ok": true, "captured": N, "sent": N, "dropped": N}` |
| `DEAUTH` | `{"bssid": "...", "channel": N, "count": N, ...}` | `{"ok": true, "sent": N}` |
| `DEAUTH_CAPTURE` | same as DEAUTH | `{"ok": true, "sent": N, "sniffing": true}` |

---

## Project Structure

```
nrcap32/
├── firmware/
│   ├── platformio.ini            # multi-board build config (C3/S3/S2)
│   ├── src/
│   │   ├── main.cpp              # Arduino entry point, CMD dispatcher
│   │   ├── sniffer.cpp / .h      # Promiscuous capture, radiotap builder
│   │   └── override_sanity.cpp   # Bypass IDF raw frame sanity check
│   └── lib/
│       └── BridgeProtocol/       # Framed binary protocol (ESP32 side)
├── nrcap32                       # Main CLI (chmod +x, run as ./nrcap32)
├── protocol.py                   # FrameParser + Protocol class
├── receiver.py                   # USB bulk IN background thread
├── sender.py                     # Thread-safe USB bulk OUT
├── usb_device.py                 # Device detection, fd wrapping, endpoints
├── pcap_writer.py                # pcap writer, buffered + stream modes
├── data/                         # Logs and captures (auto-created, gitignored)
└── README.md
```

---

## Design Notes

A few non-obvious implementation choices documented for contributors:

**Single radio mode.** The ESP32 stays in `WIFI_MODE_APSTA` permanently after `setup()`. Mode switching takes 3–8 seconds and can silently fail, so `SCAN_WIFI` calls `esp_wifi_scan_start()` directly instead. `WiFi.mode()` is called exactly once.

**USB DATA toggle.** Each new `termux-usb` session wraps the same physical device via a fresh file descriptor, but the USB DATA toggle bit in the endpoint hardware is *not* reset. `reset_endpoint_toggles()` calls `device.clear_halt()` on both endpoints after claiming the interface, resetting to DATA0. Without this, every second run silently misdelivers the first bulk transfer.

**Single endpoint owner.** `ReceiverThread` is the sole caller of `device.read()`. Concurrent `libusb_bulk_transfer` calls on the same endpoint from two threads without a mutex corrupt data or lock up the endpoint entirely on Android's USB host stack.

**Flow control.** The ESP32's `processQueue()` only dequeues PCAP frames when `_inFlight < SNIFF_MAX_INFLIGHT` (default 4). The Python side sends ACKs from the `_read_loop` thread directly — not the callback thread — to keep round-trip latency minimal.

---

## Troubleshooting

**Permission dialog doesn't appear**
Make sure the Termux:API companion app (from F-Droid, not Play Store) is installed. Run `termux-usb -l` to verify it can see devices.

**`No USB devices found`**
Some cables are charge-only — use a data-capable OTG cable. Run `termux-usb -l` (no-root) or `lsusb` (root) to confirm the phone sees the ESP32.

**Second run returns None / 30s timeout**
This is the USB DATA toggle bug. Fixed in current versions via `reset_endpoint_toggles()`. On older versions, unplug/replug the ESP32 between runs as a workaround.

**`cdc_acm: device not accepting address`** (NetHunter)
The kernel CDC driver claimed the interface first. Run `rmmod cdc_acm` once. NRcap32 will automatically detach it on subsequent runs.

**Capture shows 0 EAPOL frames**
The client didn't reassociate during the capture window. Try `--count` and `--capture-secs` higher values. Some devices ignore broadcast deauth — use `--client <MAC>` to target a specific station. Verify you're on the correct channel.

**Heap drops below 150 KB on STATUS**
Reduce `SNIFF_QUEUE_DEPTH` or `SNIFF_MAX_INFLIGHT` in `sniffer.h` and reflash. Below 80 KB, JSON serialization can fail silently.

---

## Roadmap

- [ ] AP Mode — Captive Portal support
- [ ] Beacon Broadcasting — beacon spam, custom SSIDs, hidden SSIDs
- [ ] WPS Support — WPS Pixie Dust attack
- [ ] WPA3 PMKID capture — passive, no deauth required
- [ ] Bluetooth — BLE scanning, advertising, device discovery, BLE spam
- [ ] HID Emulation — BLE BadUSB, keyboard injection
- [ ] Multi-ESP32 — USB hub + multiple ESP32s for simultaneous multi-channel capture
- [ ] Hardware expansion — IR, NRF24, CC1101 modules

---

## Legal

This tool is intended for authorized security research, penetration testing on your own infrastructure, and educational use only. The authors are not responsible for misuse.

Sending deauthentication frames disrupts connectivity for affected clients. Do not use on networks you do not own or manage. In many jurisdictions, unauthorized disruption of network communications is a criminal offense.

Always obtain explicit written permission before testing any network or device you do not own.

---

## License

MIT — see [`LICENSE`](LICENSE).

---

## Acknowledgements

- [Bruce firmware](https://github.com/pr3y/Bruce) — ESP32 multi-tool firmware; referenced for radio patterns and IR implementation
- [esp32-deauther](https://github.com/SpacehuhnTech/esp8266_deauther) — original concept inspiration
- [Espressif IDF team](https://github.com/espressif/esp-idf) — for the raw `esp_wifi_*` API that makes promiscuous mode and frame injection possible without kernel drivers
- [Termux](https://termux.dev) and termux-api contributors — for making Android a real Linux environment
- [PyUSB / libusb](https://github.com/pyusb/pyusb) — for `libusb_wrap_sys_device`, the key primitive that makes no-root USB access possible