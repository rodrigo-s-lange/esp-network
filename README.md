# esp_network

WiFi Station layer for the ESP32 family with NVS persistence, mDNS and runtime AT command configuration.

## Dependencies

> **Requires [rodrigo-s-lange/esp_at](https://github.com/rodrigo-s-lange/esp-at)**
> `esp_at_init()` must be called before `esp_network_init()`.

## Features

- WiFi STA credentials persisted in NVS (namespace `"net"`)
- Static IP or DHCP — automatically selected based on NVS content
- mDNS hostname (`AT+mDNS="fence"` → `fence.local`)
- Custom netif hostname (`AT+VENDOR`)
- Non-blocking reconnect via `esp_timer` — event loop never blocked
- Up to 5 automatic reconnect attempts (3 s interval)

## Targets

`esp32` · `esp32-c3` · `esp32-c6` · `esp32-s3`

## Usage

```c
#include "esp_at.h"
#include "esp_network.h"

nvs_flash_init();
esp_at_init();
esp_network_init();
esp_network_start();
```

## AT Commands

| Command | Effect |
|---|---|
| `AT+WIFI?` | Status, IP, RSSI, GW, DNS, mDNS |
| `AT+WIFI_SSID="MyNet"` | Save SSID |
| `AT+WIFI_SSID` | Show current SSID |
| `AT+WIFI_PWD="pass"` | Save password |
| `AT+WIFI_IP="192.168.1.10"` | Static IP (all three required for static) |
| `AT+WIFI_GW="192.168.1.1"` | Gateway |
| `AT+WIFI_SUB="255.255.255.0"` | Subnet mask |
| `AT+mDNS="fence"` | Set mDNS hostname (applied immediately) |
| `AT+VENDOR="SawFence"` | Set network hostname (applied on next boot) |

## NVS Keys (namespace `"net"`)

| Key | Type | Description |
|---|---|---|
| `ssid` | string | AP SSID |
| `pwd` | string | AP password |
| `ip` | string | Static IP (empty = DHCP) |
| `gw` | string | Gateway |
| `sub` | string | Subnet mask |
| `mdns` | string | mDNS hostname |
| `vendor` | string | Network hostname |

## Install

```bash
idf.py add-dependency "rodrigo-s-lange/esp_network>=1.0.0"
idf.py add-dependency "rodrigo-s-lange/esp_at>=1.0.0"
```

Or as git submodules:

```bash
git submodule add https://github.com/rodrigo-s-lange/esp-at.git      components/esp_at
git submodule add https://github.com/rodrigo-s-lange/esp-network.git  components/esp_network
```

## License

MIT — © 2026 Rodrigo S. Lange
