---

## 2. `CHANGELOG.md` (integrale, con v1.0.0)

```markdown
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-10-08

First stable public release.

### Added

- **ARP scanner**: periodic network scan with configurable interval (1-60 min)
- **MAC lists**: whitelist, blacklist and unknown devices with persistent
  storage in LittleFS (JSON format), descriptions, hybrid timestamps
  (Unix or uptime)
- **Telegram notifications**: aggregated message per scan, retry with
  backoff, test button with temporary credentials (no need to save first)
- **Generic webhook**: HTTP POST with JSON payload, compatible with
  ntfy.sh, Discord, Slack, IFTTT, Home Assistant, Gotify and custom servers
- **Backup / Restore**: export and import MAC lists as JSON file
- **Bulk move**: move all MACs from one list to another in a single action
- **Vendor OUI lookup**: table of ~60 common manufacturers (Apple, Samsung,
  Xiaomi, Huawei, Espressif, Raspberry Pi, etc.)
- **Web UI**: responsive interface with session cookie authentication,
  password change on first login, lockout after 5 failed attempts
- **Log viewer**: in-RAM ring buffer (2 KB) for WARNING and ERROR messages,
  accessible from the web UI
- **mDNS**: device announced as `network-watcher.local`
- **SNTP**: automatic time synchronization without dedicated task
- **LED status**: 7 patterns (off, on, blink slow, blink fast, heartbeat,
  error, scan)
- **Physical button**: short press (LED pulse), 5s long press (WiFi
  credential reset), 3+1 sequence (factory reset)
- **Factory reset**: via physical button or web UI
- **WiFi**: AP mode for initial setup, STA mode with retry policy
  (5s → 30s → 5min, infinite)

### Changed

- Web UI fully translated to English
- Firmware logs translated to English
- License changed from MIT to Apache License 2.0
- Version bumped to 1.0.0

### Hardware

- ESP32-S2 mini (WEMOS), 4 MB flash, 2 MB PSRAM
- LED: GPIO 15 (active high)
- Button: GPIO 0 (BOOT, active low)

### Notes

- Tested with ESP-IDF v6.1
- USB CDC console: use Tera Term on COM6 (`idf.py monitor` has known issues)
- Flash with BOOT+RST on COM3

### Fixed

- **fix** /index.html + /favicon.ico.

### Removed

- Web flasher (ESP Web Tools / Adafruit WebSerial ESPTool) — Web Serial
  is not compatible with the ESP32-S2 native USB CDC controller on
  Windows. Flash with `idf.py -p COMx flash` instead.
  
  
 

  