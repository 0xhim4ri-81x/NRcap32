# nrcap32

nrcap32 turns a $3–5 ESP32 into an external Wi-Fi adapter for Termux, without requiring root access. It bypasses Android's locked-down Wi-Fi API entirely by offloading the radio layer to the ESP32 (connected via USB OTG) and communicating over a custom framed binary protocol through USB CDC. The result is monitor mode, passive packet capture, radiotap-annotated pcap output, and EAPOL handshake capture — all on a stock, unrooted Android phone.

> **For authorized use only.** Only use this tool on networks and devices you own or have explicit written permission to test. Unauthorized interception of network traffic is illegal in most jurisdictions.

---

## Why this exists

Android has never exposed monitor mode or raw packet injection to user-space applications — not even with root on most devices. Getting there traditionally required a rooted phone, a custom kernel (e.g. NetHunter), a supported external USB Wi-Fi adapter, and a lot of luck matching hardware versions.

nrcap32 sidesteps the entire problem:

- The ESP32 handles everything radio-level: promiscuous capture, channel hopping, raw frame injection, EAPOL filtering
- Termux talks to it over USB CDC using `termux-usb` + libusb — no root, no kernel modules, no custom ROM
- The Python bridge speaks a compact framed binary protocol with flow control, not serial ASCII

The total hardware cost is under $5. It works on any Android phone with a USB-C or micro-USB port and a OTG cable.

---

## Features

- **Wi-Fi network scan** — active scan with SSID, BSSID, channel, RSSI, and security type
- **Packet capture** — fixed channel or channel-hopping promiscuous capture, output as `.pcap` with radiotap headers (Wireshark-compatible)
- **EAPOL capture** — passive handshake capture, optionally filtered by BSSID
- **Deauth + capture** — send deauthentication frames to trigger a handshake, capture EAPOL simultaneously
- **Live streaming** — pcap output can be piped directly to termshark via named FIFO for real-time analysis
- **Heartbeat events** — ESP32 sends uptime and free heap every 5 seconds for connection health monitoring
- **Modular firmware** — designed to be extended with new CMDs; IR, NRF24, and CC1101 modules planned

---

## Hardware

| Component | Status | Notes |
|-----------|--------|-------|
| ESP32-C3 SuperMini | Tested | Primary target, ~$3, native USB CDC built-in |
| ESP32-S3 (any board) | Untested | More RAM/flash, native USB CDC, dual-core |
| ESP32-S2 (any board) | Untested | Single-core, native USB CDC, lower cost |
| USB OTG cable / adapter | — | USB-C OTG or micro-USB OTG depending on your phone |
| Android phone | — | Any version with USB host support; no root required |

> All supported boards use **native USB CDC** — no external USB-UART bridge chip (CP2102/CH340) needed. They appear as `303A:xxxx` on Android and communicate directly over USB.
>
> **Primary tested hardware:** ESP32-C3 SuperMini. Other boards should work with minor `platformio.ini` adjustments (see Firmware section).

The firmware uses the ESP32's built-in 2.4 GHz radio — no external antenna or adapter needed.

---

## Requirements

### Android / Termux

```bash
# Core packages
pkg update
pkg install python termux-api libusb

# Python dependencies
pip install pyusb
```

Also install the **Termux:API** companion app from F-Droid (not the Play Store version — it is outdated).

### Firmware (ESP32 side)

Built with **PlatformIO** (recommended). Arduino IDE also works but PlatformIO is the primary build system.

Dependencies (all declared in `platformio.ini`):
- `espressif32` platform with IDF 5.x
- `ArduinoJson` >= 7.x

```ini
; firmware/platformio.ini (excerpt — see full file for all board envs)
[env:esp32-c3-supermini]
platform  = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.36/platform-espressif32.zip
board     = esp32-c3-devkitm-1
framework = arduino

build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DCONFIG_AUTOSTART_ARDUINO=1
    -DCONFIG_ESP_WIFI_ENABLE_SNIFFER=1

lib_deps =
    bblanchon/ArduinoJson@^7.0.0
```

> `ARDUINO_USB_CDC_ON_BOOT=1` is required for native USB CDC on the C3 and S3. Without it `Serial` routes to the UART pins instead of USB.

Flash the firmware before running any Python commands.

---

## Setup

```bash
# Clone the repo
git clone https://github.com/0xhim4ri-81x/nrcap32
cd nrcap32
```

**Flash firmware (PlatformIO — recommended):**
```bash
cd firmware/

# ESP32-C3 SuperMini (primary tested target)
pio run -e esp32-c3-supermini --target upload

# ESP32-S3 (untested)
pio run -e esp32-s3 --target upload

# ESP32-S2 (untested)
pio run -e esp32-s2 --target upload

# Monitor serial output to confirm boot
pio device monitor --baud 115200
# Should print: ESP32_READY
```

**Verify the ESP32 is working before connecting to Android:**
```bash
# With PlatformIO serial monitor you should see heartbeat JSON every 5s:
# {"uptime":5000,"heap":250336,"type":"heartbeat"}
```

Connect the ESP32 to your Android phone via OTG cable. On the first run, Android shows a USB permission dialog — tap **OK**. The permission persists until you unplug.

**Make it executable (run as `./nrcap32` instead of `python nrcap32`):**
```bash
chmod +x nrcap32
./nrcap32 scan
```

The shebang line (`#!/bin/python3`) is already set at the top of the file.

---

## Usage

All commands follow the same pattern: the script detects the USB device, requests permission via `termux-usb`, and re-invokes itself with the granted file descriptor.

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

### Capture packets (fixed channel)

```bash
./nrcap32 sniff --channel 6
./nrcap32 sniff --channel 6 -o capture.pcap
```

### Capture packets (channel hopping)

```bash
./nrcap32 sniff --hop --interval 300
```

Hops channels 1–13 every 300 ms. Stop with `Ctrl+C` — the pcap is saved automatically.

### Open pcap in Wireshark

```bash
# On PC after transferring the file
wireshark capture.pcap

# Live via termshark on the phone (requires termshark in Termux)
mkfifo /tmp/live.pcap
termshark -i /tmp/live.pcap &
./nrcap32 sniff --channel 6 -o /tmp/live.pcap
```

### Capture EAPOL handshake (passive)

```bash
# Sniff only EAPOL frames on channel 6, filtered to one BSSID
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

Sends 15 deauthentication frames to disconnect clients from the target AP, then captures EAPOL frames for 30 seconds. The resulting pcap can be used with Hashcat or Aircrack-ng for offline WPA2 key testing.

> Only use this on your own network or with explicit written permission from the network owner.

---

## Project structure

```
nrcap32/
├── firmware/
│   ├── platformio.ini            # multi-board build config (C3/C6/S3/S2)
│   ├── src/
│   │   ├── main.cpp              # Arduino entry point, CMD dispatcher
│   │   ├── sniffer.cpp / .h      # Promiscuous capture, radiotap builder
│   │   └── override_sanity.cpp   # Bypass IDF raw frame sanity check
│   ├── lib/
│   │   └── BridgeProtocol/
│   │       ├── BridgeProtocol.cpp
│   │       └── BridgeProtocol.h  # Framed binary protocol (ESP32 side)
│   ├── include/
│   └── test/
├── nrcap32                       # Main CLI (chmod +x, run as ./nrcap32)
├── protocol.py                   # FrameParser + Protocol class
├── receiver.py                   # USB bulk IN background thread
├── sender.py                     # Thread-safe USB bulk OUT
├── usb_device.py                 # Device detection, fd wrapping, endpoints
├── pcap_writer.py                # pcap writer, buffered + stream modes
├── data/                         # Logs and captures (auto-created, gitignored)
├── README.md
└── .gitignore
```

---

## Firmware

### Supported boards

All supported boards have native USB CDC built-in — no external USB-UART chip required.

| Board | `platformio.ini` env | VID:PID | Status |
|-------|---------------------|---------|--------|
| ESP32-C3 SuperMini | `esp32-c3-supermini` | `303A:1001` | Tested |
| ESP32-S3 | `esp32-s3` | `303A:1001` | Untested |
| ESP32-S2 | `esp32-s2` | `303A:0002` | Untested |

All supported boards (C3, C6, S3, S2) use native USB CDC and show up as `303A:xxxx` on Android — no external USB-UART bridge chip required.

### Full `platformio.ini`

The full `firmware/platformio.ini` uses shared `[base]` and `[common_flags]` sections so all four board envs stay DRY. Key flags explained:

```ini
; Shared flags applied to all boards (excerpt)
build_flags =
    -DARDUINO_USB_MODE=1           ; route Serial to native USB, not UART pins
    -DARDUINO_USB_CDC_ON_BOOT=1    ; enable CDC before setup() runs
    -DCONFIG_AUTOSTART_ARDUINO=1   ; critical — loop() won't run without this
    -DCONFIG_ESP_WIFI_ENABLE_SNIFFER=1
    -DCONFIG_ESP_WIFI_SOFTAP_SUPPORT=1
    -DCORE_DEBUG_LEVEL=0           ; 0=off, 3=info, 5=verbose
```

> **`ARDUINO_USB_CDC_ON_BOOT=1` is critical** — without it `Serial` maps to UART0 (TX/RX pins) and the bridge protocol gets no data over USB.

> **Important for C3 and S3:** `ARDUINO_USB_CDC_ON_BOOT=1` routes `Serial` to the native USB CDC interface. Without it `Serial` maps to UART0 (TX/RX pins) and the bridge protocol gets no data.

### Flashing

```bash
cd firmware/

# Flash + open serial monitor in one command
pio run -e esp32-c3-supermini --target upload && pio device monitor

# Just flash
pio run -e esp32-c3-supermini --target upload

# Just monitor (confirm ESP32_READY prints on boot)
pio device monitor --baud 115200
```

### Pre-built binaries

Pre-built `.bin` files for each supported board are available on the [Releases](https://github.com/0xhim4ri-81x/nrcap32/releases) page for users who do not want to build from source.

Flash with `esptool.py`:
```bash
pip install esptool

# ESP32-C3
esptool.py --chip esp32c3 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-c3.bin

# ESP32-S3
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-s3.bin

# ESP32-S2
esptool.py --chip esp32s2 --port /dev/ttyUSB0 write_flash 0x0 nrcap32-s2.bin
```

Or use the [ESP32 Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) on Windows.


---

## Bridge protocol

nrcap32 uses a compact binary framing protocol over USB CDC bulk transfer. Each frame is:

```
[0xAD 0xDE][TYPE 1B][ID 1B][LENGTH 4B LE][PAYLOAD NB]
```

| Type | Hex | Direction | Payload |
|------|-----|-----------|---------|
| CMD | 0x01 | Termux → ESP32 | JSON `{"cmd": "...", "args": {...}}` |
| RESP | 0x02 | ESP32 → Termux | JSON response, same ID as CMD |
| EVENT | 0x03 | ESP32 → Termux | JSON async event (scan results, heartbeat) |
| PCAP | 0x04 | ESP32 → Termux | Raw binary: radiotap header + 802.11 frame |
| ACK | 0x05 | Termux → ESP32 | JSON `{"chunk": N}` — flow control for PCAP |

PCAP frames use a sliding-window flow control: the ESP32 buffers up to `SNIFF_MAX_INFLIGHT` (default 4) unacknowledged PCAP frames before pausing. The Python side ACKs each frame from the protocol read loop, keeping round-trip latency minimal.

### Supported CMDs

| CMD | Args | Response |
|-----|------|----------|
| `PING` | — | `{"ok": true, "msg": "pong"}` |
| `STATUS` | — | `{"ok": true, "uptime": ms, "heap": bytes, "chip": "...", "sniffing": bool}` |
| `ECHO` | `{"msg": "..."}` | `{"ok": true, "echo": "..."}` |
| `SCAN_WIFI` | — | `{"ok": true, "count": N}` + N `scan_ap` events |
| `START_SNIFF` | `{"mode": "fixed"\|"hop", "channel": 1-13, "interval_ms": N, "eapol_only": bool, "bssid": "XX:XX:..."}` | `{"ok": true}` |
| `STOP_SNIFF` | — | `{"ok": true, "captured": N, "sent": N, "dropped": N}` |
| `SET_CHANNEL` | `{"channel": 1-13}` | `{"ok": true}` |
| `DEAUTH` | `{"bssid": "...", "channel": N, "client": "...", "count": N, "deauth_interval_ms": N, "reason": N}` | `{"ok": true, "sent": N}` |
| `DEAUTH_CAPTURE` | same as DEAUTH | `{"ok": true, "sent": N, "sniffing": true}` |

### Events

| Event type | Payload fields |
|-----------|---------------|
| `heartbeat` | `uptime` (ms), `heap` (bytes free) |
| `scan_ap` | `ssid`, `bssid`, `channel`, `rssi`, `security` |

---

## Environments

nrcap32 supports three runtime environments, detected automatically at startup via `usb_device.detect_backend()`. The same CLI commands work identically in all three — the only difference is how the USB device is opened.

### 1. Termux (no root) — recommended for most users

Stock Android with Termux + Termux:API installed. USB access goes through Android's USB host permission system via `termux-usb`. A permission dialog appears on first use per device; grant it once and it persists until you unplug.

```
Android USB host stack
        ↓
   termux-usb  (requests permission, grants fd)
        ↓
libusb_wrap_sys_device(fd)  ← wraps Android fd, no /dev/bus access needed
        ↓
   nrcap32 Python
```

**Setup:**
```bash
pkg install python termux-api libusb
pip install pyusb
# Also install Termux:API companion app from F-Droid (not Play Store)
```

**Run:**
```bash
./nrcap32 scan
# Android permission popup appears on first run — tap OK
```

No root. No custom kernel. Works on any unmodified Android phone.

---

### 2. Termux with root (Termux:Root / sudo)

When running as uid 0, `detect_backend()` returns `'root'` and libusb opens `/dev/bus/usb` directly — no `termux-usb`, no Android permission dialog, no two-process bootstrap. Cleaner and faster.

```
/dev/bus/usb/XXX/YYY  ← libusb opens directly
        ↓
   nrcap32 Python
```

**Setup:**
```bash
pkg install python libusb
pip install pyusb
# No Termux:API needed
```

**Run:**
```bash
tsu                        # or: sudo ./nrcap32 scan
./nrcap32 scan
```

The `termux-usb` bootstrap step is skipped entirely — the script goes straight to the command handler.

---

### 3. Kali NetHunter

NetHunter runs a Kali chroot on top of Android. nrcap32 works in both the NetHunter terminal (chroot, typically root) and in a Termux session running alongside it.

**In the NetHunter terminal (Kali chroot, root):**
```bash
apt update && apt install python3 python3-pip libusb-1.0-0
pip3 install pyusb
./nrcap32 scan
# libusb opens /dev/bus/usb directly — no permission dialog
```

**In Termux alongside NetHunter (no root):**
Same as the standard Termux no-root setup above. Both environments can coexist — they just cannot hold the USB device simultaneously.

**NetHunter note:** If the `cdc_acm` kernel module has claimed the ESP32's CDC interface, `claim_device()` detaches it automatically via `libusb_detach_kernel_driver`. No manual `rmmod` needed.

---

### Backend detection summary

| Environment | uid | Backend | USB access method |
|---|---|---|---|
| Stock Termux (no root) | 1000 | `termux` | `termux-usb` → `libusb_wrap_sys_device(fd)` |
| Termux + root (tsu/sudo) | 0 | `root` | libusb → `/dev/bus/usb` directly |
| NetHunter terminal (Kali chroot) | 0 | `root` | libusb → `/dev/bus/usb` directly |
| NetHunter + Termux side-by-side (no root) | 1000 | `termux` | `termux-usb` → `libusb_wrap_sys_device(fd)` |

Detection is fully automatic — you never need to pass a `--root` flag or configure anything.


---

## Comparison

| | nrcap32 (no root) | nrcap32 (root) | NetHunter + ext. adapter |
|--|:-----------------:|:--------------:|:------------------------:|
| Root required | ✗ | ✓ | ✓ |
| Custom kernel / ROM | ✗ | ✗ | ✓ |
| Hardware cost | ~$5 | ~$5 | $150–400+ |
| Supported devices | Any Android | Any Android | NetHunter-supported only |
| Monitor mode | ✓ (ESP32) | ✓ (ESP32) | ✓ |
| Packet injection | ✓ | ✓ | ✓ |
| USB permission dialog | First use only | ✗ | ✗ |
| Live Wireshark (FIFO) | ✓ | ✓ | ✓ |
| Multi-channel capture | Planned | Planned | Adapter-dependent |
| IR / RF expansion | Planned | Planned | ✗ |

The root path is strictly faster and simpler — no subprocess bootstrap, no permission dialog, no `termux-usb` overhead. But the no-root path is the unique capability: it turns any stock Android phone into a Wi-Fi capture platform with zero setup beyond installing Termux and a $5 chip.

---

## Design notes

**Single radio mode.** The ESP32 stays in `WIFI_MODE_APSTA` permanently after `setup()`. Mode switching (`APSTA → STA → APSTA`) between scans tears down the IDF WiFi task, takes 3–8 seconds, and can silently fail. Instead, `SCAN_WIFI` calls `esp_wifi_scan_start()` directly — the IDF API has no mode restriction — and `DEAUTH` updates the AP interface channel in-place without a mode switch. `WiFi.mode()` is called exactly once, in `setup()`.

**USB DATA toggle.** Each new `termux-usb` session wraps the same physical USB device via a new file descriptor. The USB DATA toggle bit lives in the endpoint hardware and is **not** reset when the fd is re-opened between runs. `usb_device.reset_endpoint_toggles()` calls `device.clear_halt()` on both the IN and OUT endpoints after claiming the interface, resetting the toggle to DATA0. Without this, every second run silently misdelivers the first bulk transfer and the CMD frame never reaches the ESP32.

**Single endpoint owner.** `ReceiverThread` is the sole caller of `device.read()`. No other thread — including `drain_stale()` — touches the endpoint directly. Concurrent `libusb_bulk_transfer` calls on the same endpoint from two threads without a mutex cause Android's USB host stack to return corrupt data or lock up the endpoint entirely. `drain_stale()` only empties the software queues; the receiver thread handles the hardware FIFO through its normal poll loop.

**Flow control.** The ESP32's `processQueue()` only dequeues and sends PCAP frames when `_inFlight < SNIFF_MAX_INFLIGHT` (default 4). Each `ACK` frame from Termux decrements the counter. The Python side sends ACKs directly from the `_read_loop` thread — not via the callback thread — to keep round-trip time minimal. This prevents the 512-entry capture queue from overflowing during fast captures while still keeping 4 frames pipelined for throughput.

**Two-process bootstrap (no-root only).** Android's USB permission system requires the permission grant to happen in a process that already owns the file descriptor. `bootstrap()` spawns `termux-usb -r -E -e "python nrcap32 <cmd>" <device>`, which requests permission, then re-execs the script with `TERMUX_USB_FD` set. The parent redirects the child's stdout/stderr to the log file and tails it — `tail_log()` is the only thing printing to the terminal, preventing duplicate output. In root mode this entire dance is skipped.

---

## Roadmap
- [ ] AP Mpde Support — Captive Portal
- [ ] Beacon Broadcasting — Beacon Spam, Custom SSIDs, Hidden SSIDs
- [ ] WPS Support — WPS Pixie Dust
- [ ] Bluetooth Support — BLE Scanning, Advertising, Device Discovery, BLE spam
- [ ] HID Emulation — BLE BadUSB, Keyboard Injection
- [ ] Multi-ESP32 — USB hub + multiple ESP32s for simultaneous multi-channel capture
- [ ] WPA3 PMKID capture — passive, no deauth required
- [ ] Hardware Support — ESP32 Variants and Expansion Devices


---

## Troubleshooting

**Permission dialog doesn't appear**
Make sure the Termux:API companion app (from F-Droid, not Play Store) is installed and up to date. Run `termux-usb -l` to verify it can see devices.

**`No USB devices found`**
Check that the OTG cable/adapter is working. Some cables are charge-only — use a data-capable OTG cable. Run `lsusb` (root) or `termux-usb -l` (no-root) to confirm the phone sees the ESP32.

**Second run returns None / 30s timeout**
This is the USB DATA toggle bug. The fix is `reset_endpoint_toggles()` called after `claim_device()` in `usb_device.py`. If you are on an older version, unplug/replug the ESP32 between runs as a workaround.

**`cdc_acm: device not accepting address`** (NetHunter)
The kernel CDC driver claimed the interface before libusb could. Run `rmmod cdc_acm` once, or add `ACTION=="add", ATTRS{idVendor}=="303a", RUN+="/bin/sh -c 'echo -n %k > /sys/bus/usb/drivers/cdc_acm/unbind'"` to a udev rule.

**Capture shows 0 EAPOL frames**
The client did not reassociate during the capture window. Try increasing `--count` and `--capture-secs`. Some devices ignore broadcast deauth — use `--client <MAC>` to target a specific station. Make sure you are on the correct channel with `--channel`.

**Heap drops below 150 KB on STATUS**
Reduce `SNIFF_QUEUE_DEPTH` or `SNIFF_MAX_INFLIGHT` in `sniffer.h` and reflash. At 150 KB free the ESP32 will still function; below 80 KB JSON serialization can fail silently.

---

## Legal

This tool is intended for authorized security research, penetration testing on your own infrastructure, and educational use only. The authors are not responsible for misuse. Always obtain explicit written permission before testing any network or device you do not own.

Sending deauthentication frames (`DEAUTH`, `DEAUTH_CAPTURE`) disrupts connectivity for affected clients. Do not use on networks you do not own or manage. In many jurisdictions, unauthorized disruption of network communications is a criminal offense.

---

## License

MIT License — see `LICENSE` file.

---

## Acknowledgements

- [Bruce firmware](https://github.com/pr3y/Bruce) — ESP32 multi-tool firmware, referenced for radio patterns and IR implementation
- [esp32-deauther](https://github.com/SpacehuhnTech/esp8266_deauther) — original concept inspiration
- Espressif IDF team — for the raw `esp_wifi_*` API that makes promiscuous mode and frame injection possible without kernel drivers
- Termux and termux-api contributors — for making Android a real Linux environment
- PyUSB / libusb contributors — for `libusb_wrap_sys_device`, the key primitive that makes no-root USB access possible