# esp_network

WiFi Station layer for the ESP32 family with NVS persistence, mDNS, WebTerm and synchronized time support.

## Dependencies

> **Requires [rodrigo-s-lange/esp_at](https://github.com/rodrigo-s-lange/esp-at)**
> `esp_at_init()` must be called before `esp_network_init()`.

## Features

- WiFi STA credentials persisted in NVS (namespace `"net"`)
- Static IP or DHCP, selected automatically from stored config
- mDNS hostname (`AT+mDNS="fence"` -> `fence.local`)
- Custom netif hostname (`AT+VENDOR`)
- Non-blocking reconnect via `esp_timer`
- Optional WebTerm (HTTP + WebSocket)
- SNTP via `esp_netif_sntp`
- POSIX timezone support for local-time scheduling
- AT commands for time, timezone and NTP status

## Usage

```c
ESP_ERROR_CHECK(esp_network_init());
ESP_ERROR_CHECK(esp_network_start());
ESP_ERROR_CHECK(esp_network_set_timezone("<-03>3"));
ESP_ERROR_CHECK(esp_network_time_init());
```

If SNTP init fails, keep the firmware running and log the error:

```c
esp_err_t time_err = esp_network_time_init();
if (time_err != ESP_OK) {
    ESP_LOGW("main", "network time init falhou: %s", esp_err_to_name(time_err));
}
```

## AT Commands

| Command | Effect |
|---|---|
| `AT+WIFI?` | Status, IP, RSSI, GW, DNS, mDNS |
| `AT+WIFI_SSID="MyNet"` | Save SSID |
| `AT+WIFI_PWD="pass"` | Save password |
| `AT+WIFI_IP="192.168.1.10"` | Static IP |
| `AT+WIFI_GW="192.168.1.1"` | Gateway |
| `AT+WIFI_SUB="255.255.255.0"` | Subnet mask |
| `AT+mDNS="fence"` | Set mDNS hostname |
| `AT+VENDOR="SawFence"` | Set network hostname |
| `AT+TIME?` | Show SNTP state, timezone, local and UTC time |
| `AT+TZ="<-03>3"` | Set timezone immediately and persist in NVS |
| `AT+NTP?` | Show SNTP state and server reachability |

## Time API

| Function | Effect |
|---|---|
| `esp_network_time_init()` | Start SNTP with default servers |
| `esp_network_time_deinit()` | Stop SNTP support |
| `esp_network_time_is_synced()` | Check whether system time is valid |
| `esp_network_time_wait_sync(ms)` | Wait for first sync |
| `esp_network_set_timezone("...")` | Apply POSIX TZ string |
| `esp_network_get_local_time(&tm)` | Read current local time |
| `esp_network_get_utc_time(&tm)` | Read current UTC time |

Default SNTP servers:

- `pool.ntp.org`
- `time.google.com`

The component automatically limits the configured server count to `CONFIG_LWIP_SNTP_MAX_SERVERS`, so builds with only one enabled server still boot normally.

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
| `tz` | string | POSIX timezone |

## License

MIT - © 2026 Rodrigo S. Lange
