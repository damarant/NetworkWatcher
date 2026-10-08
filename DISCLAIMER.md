# Disclaimer

**NetworkWatcher** is a network monitoring tool for home use, running on
low-cost ESP32-S2 hardware. Before using it, read this disclaimer carefully.

## 1. Intended use

NetworkWatcher is designed for **monitoring your own local network**.
Its primary purpose is to help home users detect new or unauthorized
devices on their LAN by scanning the ARP table and notifying them.

## 2. Cybersecurity disclaimer

### 2.1 Use only on networks you own or are authorized to monitor

Network scanning **without explicit authorization is illegal** in many
jurisdictions, including (but not limited to) the European Union, the
United States, and the United Kingdom. You **must not** use this software:

- On networks you do not own
- On networks where you do not have written permission from the owner
- On public, corporate, or shared networks without authorization
- To perform any kind of unauthorized reconnaissance, penetration testing,
  or intrusion against third parties

**You are solely responsible** for ensuring that your use of NetworkWatcher
complies with all applicable laws and regulations in your country.

### 2.2 No security guarantees

NetworkWatcher is provided for **informational purposes only**. It is **not**
a substitute for professional security auditing tools, intrusion detection
systems, or firewalls. The detection capabilities of this tool are limited
by:

- The ARP-based scanning method (which only sees devices that respond
  to ARP requests within the local subnet)
- Random MAC address features on modern devices (iOS, Android, Windows)
- The device's own hardware and software limitations

The absence of a notification **does not mean** that your network is safe.
The presence of a notification **does not prove** that a device is
malicious or unauthorized.

### 2.3 Data handling

NetworkWatcher stores information **locally** on the device:
- MAC addresses of detected devices
- Vendor names derived from MAC OUI
- Timestamps of first and last detection
- Optional Telegram bot token and chat ID
- Optional generic webhook URL and Bearer token

No data is transmitted to third parties by the firmware itself, except:
- Telegram notifications sent to the Telegram API (if configured by the user)
- Webhook notifications sent to the user-configured URL
- Optional NTP requests for time synchronization

**You are responsible** for securing access to:
- The device's web interface (change the default admin password)
- The Telegram bot token and webhook URL (they grant access to your
  notification channels)
- The device's Wi-Fi credentials

## 3. Hardware disclaimer

### 3.1 Use at your own risk

This firmware is provided **as-is**, without any warranty of any kind,
express or implied. The author is **not liable** for any damage, direct
or indirect, caused to:

- The ESP32-S2 board or any other hardware
- Your computer, router, or network equipment
- Your network configuration or connected devices
- Data stored on any device

### 3.2 Power supply

The WEMOS S2 Mini and similar ESP32-S2 boards require a **stable 5V USB
power supply**. Insufficient or unstable power may cause:

- Random reboots
- Flash corruption
- Failure to enumerate the USB CDC console
- In the worst case, permanent hardware damage

**Always use a quality power source.** Do not power the board from
unreliable USB hubs or low-current chargers.

### 3.3 Flashing

Flashing custom firmware modifies the device. If you are not familiar
with the process:

- Read the ESP-IDF documentation first
- Make a backup of the original firmware if you intend to restore it
- Be aware that a **full flash** (`idf.py flash`) will overwrite the
  `storage` partition and delete any saved MAC lists

### 3.4 Continuous operation

NetworkWatcher is designed for **continuous operation**, but like all
electronic devices, it may fail. Do not rely on it as the sole
protection for your network. Do not use it in life-critical or
safety-critical applications.

## 4. Legal notes

### 4.1 License

NetworkWatcher is released under the **Apache License 2.0**.
See the [LICENSE](LICENSE) file for the full text.

### 4.2 No warranty

Unless required by applicable law or agreed to in writing, the software
is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF
ANY KIND, either express or implied. See the Apache License 2.0, Section 7
for details.

### 4.3 Limitation of liability

In no event and under no legal theory shall the author be liable for any
damages arising from the use or inability to use this software. See the
Apache License 2.0, Section 8 for details.

### 4.4 Third-party trademarks

WEMOS, ESP32, ESP32-S2 and all other trademarks mentioned in this project
are the property of their respective owners. This project is **not
affiliated with, endorsed by, or sponsored by** any of the trademark
holders.

## 5. Compliance checklist

Before using NetworkWatcher, ask yourself:

- [ ] Do I **own** the network I want to monitor, or do I have **written
      permission** from its owner?
- [ ] Am I aware of the laws in my country regarding network scanning?
- [ ] Have I changed the default admin password (`admin` / `admin`)?
- [ ] Have I secured my Telegram bot token and/or webhook URL?
- [ ] Have I read and understood the Apache License 2.0 terms?
- [ ] Do I understand that this tool provides **informational data only**
      and is **not a security guarantee**?

If you answered **no** to any of these questions, **do not use**
NetworkWatcher until you have addressed them.

## 6. Contact

For questions, bug reports, or security concerns, open an issue on the
project repository:

<https://github.com/damarant/NetworkWatcher/issues>

---

**By using NetworkWatcher, you acknowledge that you have read,
understood, and agreed to this disclaimer.**