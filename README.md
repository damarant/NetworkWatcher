# NetworkWatcher

Home network monitoring system running on an ESP32-S2 mini (WEMOS).
It performs periodic ARP scans, keeps track of devices, and sends
notifications when new or blacklisted devices appear on the network.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

## 🚀 Easy Install (no tools required)

Flash NetworkWatcher directly from your browser:

👉 **[Install NetworkWatcher](https://damarant.github.io/NetworkWatcher/)**

Works on Chrome, Edge or Opera (desktop). No drivers, no Python, no `esptool`.
The flasher writes both the firmware and the web UI in one click.

> ⚠️ A full flash will overwrite the `storage` partition and erase your MAC lists.
> Back them up from the web UI first if needed.

## Features

- **ARP scanner** — periodic network scan (configurable interval 1-60 min)
- **MAC lists** — whitelist, blacklist and unknown devices, with descriptions
- **Telegram notifications** — aggregated message per scan
- **Generic webhook** — HTTP POST to ntfy.sh, Discord, Slack, IFTTT, Home
  Assistant, Gotify, or any custom server
- **Backup / restore** — export and import MAC lists as JSON
- **Bulk move** — move all MACs from one list to another
- **Vendor lookup** — manufacturer name from MAC OUI (~60 vendors)
- **Web UI** — responsive interface with session authentication
- **Log viewer** — in-RAM buffer for WARNING and ERROR messages
- **mDNS** — `network-watcher.local`
- **Factory reset** — via button or web UI

## Hardware

| Component | Details |
|---|---|
| Board | ESP32-S2 mini (WEMOS) |
| Flash | 4 MB |
| PSRAM | 2 MB |
| LED | GPIO 15, active high |
| Button | GPIO 0 (BOOT), active low |
| Console | USB CDC (native USB-OTG) |

## Requirements (for development)

- [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/)
- Python 3.12+
- Tera Term (or PuTTY) for the serial console on Windows

## Build and Flash

### 1. Build

```bash
cd NetworkWatcher
idf.py build
```

### 2. Find your serial ports

The board exposes **two different COM ports** depending on the mode:

powershell

Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Name

|Mode|Buttons|Port (example)|
|---|---|---|
|Download (flashing)|Hold **BOOT**, press and release **RST**, then release **BOOT**|`COM3` (UART bridge)|
|Console (USB CDC)|Press **RST** only|`COM6` (native USB CDC)|

> ⚠️ The exact port numbers vary from system to system. Replace `COM3` and  
> `COM6` in the commands below with the ports found on your machine.

### 3. Flash

1. Hold **BOOT**, press and release **RST**, then release **BOOT** (download mode)
2. Run (replace `COM3` with your download port):

```bash
idf.py -p COM3 flash
```

### 4. Monitor

1. Press **RST** only
2. Open **Tera Term** on the console port (example `COM6`), 115200, 8N1,  
auto-reconnect enabled

> The native USB CDC console requires Tera Term or PuTTY.  
> `idf.py monitor` has known issues on Windows with USB CDC.  
> The port may change number after a flash: re-check with the command above.

## First-time setup

1. After flashing, the device starts in **Access Point mode**:
    - SSID: `Network Watcher`
    - Password: `12345678`
2. Connect to the AP and open `http://192.168.4.1`
3. Login with **admin / admin**
4. Change the password when prompted (mandatory)
5. Configure your home WiFi from the **Setup** page
6. (Optional) Configure Telegram bot and/or generic webhook for notifications

## How to access the device

After the first-time setup, the device is reachable at:

|Method|Address|Notes|
|---|---|---|
|mDNS|`http://network-watcher.local`|Works on macOS, iOS, Linux. On Windows may require Bonjour|
|Direct IP|`http://<device-ip>`|Check your router's DHCP leases|

The device's IP in STA mode is shown at boot on the serial console:

 (xxxx) main: IP assigned: 192.168.1.174
 
 
You can also find it in the router's admin page under **DHCP leases** or
**Connected devices**, looking for the hostname `network-watcher` or the
board's MAC address (shown at boot).

### Recommended: assign a fixed IP (DHCP reservation)

By default the IP may change across reboots. To always reach the device
at the same address:

1. Open your router's admin page (usually `http://192.168.1.1`)
2. Find the **DHCP reservation** / **Static DHCP** / **Address reservation** section
3. Add a new entry with:
   - **MAC address**: the board's MAC (shown at boot: `wifi:mac: xx:xx:xx:xx:xx:xx`)
   - **IP address**: any free address in your LAN (e.g. `192.168.1.174`)
4. Save and reboot the device

From that point on, the router always assigns the same IP to NetworkWatcher.

### AP mode (fallback)

If the device cannot connect to your home WiFi, it stays in **Access Point mode**:

- SSID: `Network Watcher`
- Password: `12345678`
- Address: `http://192.168.4.1`

To force AP mode manually, hold **BOOT** for 5 seconds.

## Usage

Once configured, the device connects to your home WiFi and starts scanning  
every 5 minutes (configurable). When new or blacklisted devices appear, a  
notification is sent via the configured channels.

The web UI is available at:

- `http://192.168.4.1` in AP mode
- `http://network-watcher.local` in STA mode (mDNS)
- `http://<device-ip>` in STA mode

### Button behavior

|Action|Effect|
|---|---|
|Short press|LED pulse|
|5 seconds long press|Reset WiFi credentials, restart in AP mode|
|3 rapid clicks + 1 within 5s|Factory reset (all settings)|

### LED patterns

|Pattern|Meaning|
|---|---|
|Off|All lists clean|
|Slow blink|Unknown devices present|
|Solid on|Blacklist match detected|
|Fast blink|WiFi connecting|
|Heartbeat|Connected and idle|
|Error (3 blinks)|Error|
|Quick flash|Scan in progress|

## Project structure

```text

NetworkWatcher/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── LICENSE
├── NOTICE
├── DISCLAIMER.md
├── README.md
├── CHANGELOG.md
├── main/
│   └── main.c
├── components/
│   ├── config/           Shared constants and types
│   ├── storage/          NVS + LittleFS wrapper
│   ├── led/              LED patterns
│   ├── button/           Button event detection
│   ├── wifi_manager/     AP/STA management
│   ├── auth/             Session auth (SHA-256 + salt)
│   ├── mdns_service/     mDNS announcement
│   ├── web_server/       HTTP server + REST API
│   ├── arp_scanner/      ARP scan task
│   ├── mac_list/         MAC list management
│   ├── settings/         Application settings
│   ├── time_service/     SNTP sync
│   ├── telegram/         Telegram Bot API
│   ├── webhook/          Generic HTTP webhook
│   ├── oui_lookup/       MAC vendor lookup
│   ├── log_buffer/       In-RAM log ring buffer
│   └── esp_littlefs/     LittleFS library (local)
├── data/
│   └── www/              Web UI (LittleFS image)
└── docs/                 Web flasher (GitHub Pages)
    ├── index.html
    └── firmware/
        ├── manifest.json
        ├── bootloader.bin
        ├── partitions.bin
        ├── NetworkWatcher.bin
        └── storage.bin
```

## Partition layout

|Name|Type|Size|Purpose|
|---|---|---|---|
|nvs|data/nvs|24 KB|WiFi, admin, Telegram, Webhook credentials|
|phy_init|data/phy|4 KB|RF calibration|
|factory|app/factory|1.5 MB|Firmware|
|storage|data/spiffs|1 MB|LittleFS: web UI + MAC lists|
|backup|data/0x40|256 KB|(unused, reserved)|

## Notes

- MAC lists are stored in LittleFS. A full `idf.py flash` will overwrite the  
    storage partition and erase them. Use the **Backup** feature in the UI  
    before flashing a new full image, or use `idf.py app-flash` to update only  
    the firmware.
- Telegram and webhook credentials are stored in NVS, not in the storage  
    partition, so they survive a full flash.
- The Log viewer buffer is volatile: it is cleared on every reboot.

## Security

Before using NetworkWatcher, please read the [DISCLAIMER](https://disclaimer.md/).  
NetworkWatcher is intended for monitoring your **own** network. Scanning  
networks you do not own or are not authorized to monitor is illegal in  
many jurisdictions.

## License

Licensed under the **Apache License, Version 2.0**.  
See [LICENSE](https://license/) for the full text and [NOTICE](https://notice/) for  
third-party attributions.

## Author

damarant

Repository: [https://github.com/damarant/NetworkWatcher](https://github.com/damarant/NetworkWatcher)