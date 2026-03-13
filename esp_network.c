#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_at.h"
#include "esp_network.h"

#define NET_NVS_NAMESPACE   "net"
#define NET_MAX_RETRY       5
#define NET_RECONNECT_US    (3000ULL * 1000ULL)
#define NET_STR_MAX         64
#define NET_TZ_MAX          64
#define NET_DEFAULT_TZ      "UTC0"

static const char *TAG = "esp_network";

typedef struct {
    char ssid[NET_STR_MAX];
    char pwd[NET_STR_MAX];
    char ip[NET_STR_MAX];
    char gw[NET_STR_MAX];
    char sub[NET_STR_MAX];
    char mdns_host[NET_STR_MAX];
    char vendor[NET_STR_MAX];
    char tz[NET_TZ_MAX];
} net_cfg_t;

static net_cfg_t s_cfg;
static esp_netif_t *s_netif = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_init_done = false;
static bool s_time_init_done = false;
static bool s_time_synced = false;
static int s_retry = 0;
static char s_timezone[NET_TZ_MAX] = NET_DEFAULT_TZ;

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
#define NET_SNTP_SERVER_COUNT 2
#else
#define NET_SNTP_SERVER_COUNT 1
#endif

static esp_err_t _nvs_get_str(const char *key, char *dst, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, dst, &len);
    nvs_close(h);
    return err;
}

static esp_err_t _nvs_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void _nvs_load_all(void)
{
    size_t len;

    memset(&s_cfg, 0, sizeof(s_cfg));
    len = sizeof(s_cfg.ssid);      _nvs_get_str("ssid", s_cfg.ssid, len);
    len = sizeof(s_cfg.pwd);       _nvs_get_str("pwd", s_cfg.pwd, len);
    len = sizeof(s_cfg.ip);        _nvs_get_str("ip", s_cfg.ip, len);
    len = sizeof(s_cfg.gw);        _nvs_get_str("gw", s_cfg.gw, len);
    len = sizeof(s_cfg.sub);       _nvs_get_str("sub", s_cfg.sub, len);
    len = sizeof(s_cfg.mdns_host); _nvs_get_str("mdns", s_cfg.mdns_host, len);
    len = sizeof(s_cfg.vendor);    _nvs_get_str("vendor", s_cfg.vendor, len);
    len = sizeof(s_cfg.tz);        _nvs_get_str("tz", s_cfg.tz, len);

    if (s_cfg.tz[0] != '\0') {
        strncpy(s_timezone, s_cfg.tz, sizeof(s_timezone) - 1);
        s_timezone[sizeof(s_timezone) - 1] = '\0';
    }
}

static bool _time_valid(void)
{
    return time(NULL) >= 1704067200;
}

static esp_err_t _apply_timezone(const char *tz)
{
    size_t len;

    if (tz == NULL) return ESP_ERR_INVALID_ARG;

    len = strnlen(tz, NET_TZ_MAX);
    if (len == 0U || len >= NET_TZ_MAX) return ESP_ERR_INVALID_ARG;

    if (setenv("TZ", tz, 1) != 0) return ESP_FAIL;
    tzset();

    strncpy(s_timezone, tz, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    return ESP_OK;
}

static void _print_tm_line(const char *label, const struct tm *tm_info)
{
    AT(O "%s: " W "%04d-%02d-%02d %02d:%02d:%02d",
       label,
       tm_info->tm_year + 1900,
       tm_info->tm_mon + 1,
       tm_info->tm_mday,
       tm_info->tm_hour,
       tm_info->tm_min,
       tm_info->tm_sec);
}

static void _start_mdns(void)
{
    const char *host = (s_cfg.mdns_host[0] != '\0') ? s_cfg.mdns_host : "esp32";

    mdns_free();
    if (mdns_init() != ESP_OK) {
        AT_E(R "mDNS: falha ao inicializar");
        return;
    }

    mdns_hostname_set(host);
    mdns_instance_name_set((s_cfg.vendor[0] != '\0') ? s_cfg.vendor : "ESP32 Device");
    AT_I(G "mDNS: " W "%s.local", host);
}

static void _apply_static_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};

    if (s_cfg.ip[0] == '\0' || s_cfg.gw[0] == '\0' || s_cfg.sub[0] == '\0') {
        return;
    }

    if (esp_netif_str_to_ip4(s_cfg.ip, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(s_cfg.gw, &ip_info.gw) != ESP_OK ||
        esp_netif_str_to_ip4(s_cfg.sub, &ip_info.netmask) != ESP_OK) {
        AT_W(Y "IP estatico invalido - usando DHCP");
        return;
    }

    esp_netif_dhcpc_stop(s_netif);
    if (esp_netif_set_ip_info(s_netif, &ip_info) != ESP_OK) {
        AT_W(Y "Falha ao aplicar IP estatico - usando DHCP");
        esp_netif_dhcpc_start(s_netif);
    }
}

static void _reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void _sntp_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != NETIF_SNTP_EVENT || event_id != NETIF_SNTP_TIME_SYNC) {
        return;
    }

    s_time_synced = _time_valid();
    if (s_time_synced) {
        struct tm local_tm = {0};
        time_t now = time(NULL);
        localtime_r(&now, &local_tm);
        AT_I(G "Horario sincronizado: " W "%04d-%02d-%02d %02d:%02d:%02d",
             local_tm.tm_year + 1900,
             local_tm.tm_mon + 1,
             local_tm.tm_mday,
             local_tm.tm_hour,
             local_tm.tm_min,
             local_tm.tm_sec);
    }
}

static void _wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                s_retry = 0;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *ev = event_data;
                char ssid[33] = {0};

                esp_timer_stop(s_reconnect_timer);
                memcpy(ssid, ev->ssid, ev->ssid_len);
                AT_I(G "WiFi conectado - AP: " W "%s", ssid);

                if (s_cfg.vendor[0] != '\0') {
                    esp_netif_set_hostname(s_netif, s_cfg.vendor);
                }
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev = event_data;
                if (s_retry < NET_MAX_RETRY) {
                    s_retry++;
                    AT_W(Y "WiFi desconectado (reason %d) - tentativa %d/%d",
                         ev->reason,
                         s_retry,
                         NET_MAX_RETRY);
                    esp_timer_start_once(s_reconnect_timer, NET_RECONNECT_US);
                } else {
                    AT_E(R "WiFi: maximo de tentativas atingido");
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        char ip_str[16];
        char gw_str[16];

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ev->ip_info.gw));
        s_retry = 0;
        AT_I(G "IP: " W "%s  " G "GW: " W "%s", ip_str, gw_str);
        _start_mdns();
    }
}

static void _handle_wifi_status(const char *param)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip_info;
    esp_netif_dns_info_t dns_main;
    esp_netif_dns_info_t dns_backup;

    (void)param;

    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        AT(O "WiFi: " R "DISCONNECTED");
        return;
    }

    esp_netif_get_ip_info(s_netif, &ip_info);
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_main);
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);

    AT(O "WiFi: " G "CONNECTED");
    at(O "  SSID   : " W); AT(O "%s", (char *)ap.ssid);
    at(O "  RSSI   : " W); AT(C "%d dBm", ap.rssi);
    at(O "  IP     : " W); AT(W IPSTR, IP2STR(&ip_info.ip));
    at(O "  GW     : " W); AT(W IPSTR, IP2STR(&ip_info.gw));
    at(O "  Subnet : " W); AT(W IPSTR, IP2STR(&ip_info.netmask));
    at(O "  DNS1   : " W); AT(W IPSTR, IP2STR(&dns_main.ip.u_addr.ip4));
    at(O "  DNS2   : " W); AT(W IPSTR, IP2STR(&dns_backup.ip.u_addr.ip4));
    if (s_cfg.mdns_host[0] != '\0') {
        at(O "  mDNS   : " W); AT(C "%s.local", s_cfg.mdns_host);
    }
}

static void _handle_wifi_ssid(const char *param)
{
    if (param == NULL) {
        AT(O "SSID: " W "%s", s_cfg.ssid[0] ? s_cfg.ssid : "(nao configurado)");
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: SSID invalido");
        return;
    }
    strncpy(s_cfg.ssid, param, sizeof(s_cfg.ssid) - 1);
    s_cfg.ssid[sizeof(s_cfg.ssid) - 1] = '\0';
    _nvs_set_str("ssid", s_cfg.ssid);
    AT(G "SSID stored: " W "%s", s_cfg.ssid);
}

static void _handle_wifi_pwd(const char *param)
{
    if (param == NULL) {
        AT(O "PWD: " W "***");
        return;
    }
    if (strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: senha muito longa");
        return;
    }
    strncpy(s_cfg.pwd, param, sizeof(s_cfg.pwd) - 1);
    s_cfg.pwd[sizeof(s_cfg.pwd) - 1] = '\0';
    _nvs_set_str("pwd", s_cfg.pwd);
    AT(G "PWD stored");
}

static void _handle_wifi_ip(const char *param)
{
    if (param == NULL) {
        AT(O "IP estatico: " W "%s", s_cfg.ip[0] ? s_cfg.ip : "(DHCP)");
        return;
    }
    strncpy(s_cfg.ip, param, sizeof(s_cfg.ip) - 1);
    s_cfg.ip[sizeof(s_cfg.ip) - 1] = '\0';
    _nvs_set_str("ip", s_cfg.ip);
    AT(G "IP stored: " W "%s", s_cfg.ip);
}

static void _handle_wifi_gw(const char *param)
{
    if (param == NULL) {
        AT(O "GW: " W "%s", s_cfg.gw[0] ? s_cfg.gw : "(DHCP)");
        return;
    }
    strncpy(s_cfg.gw, param, sizeof(s_cfg.gw) - 1);
    s_cfg.gw[sizeof(s_cfg.gw) - 1] = '\0';
    _nvs_set_str("gw", s_cfg.gw);
    AT(G "GW stored: " W "%s", s_cfg.gw);
}

static void _handle_wifi_sub(const char *param)
{
    if (param == NULL) {
        AT(O "Subnet: " W "%s", s_cfg.sub[0] ? s_cfg.sub : "(DHCP)");
        return;
    }
    strncpy(s_cfg.sub, param, sizeof(s_cfg.sub) - 1);
    s_cfg.sub[sizeof(s_cfg.sub) - 1] = '\0';
    _nvs_set_str("sub", s_cfg.sub);
    AT(G "Subnet stored: " W "%s", s_cfg.sub);
}

static void _handle_mdns(const char *param)
{
    if (param == NULL) {
        AT(O "mDNS: " W "%s", s_cfg.mdns_host[0] ? s_cfg.mdns_host : "(nao configurado)");
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: hostname mDNS invalido");
        return;
    }
    strncpy(s_cfg.mdns_host, param, sizeof(s_cfg.mdns_host) - 1);
    s_cfg.mdns_host[sizeof(s_cfg.mdns_host) - 1] = '\0';
    _nvs_set_str("mdns", s_cfg.mdns_host);
    _start_mdns();
    AT(G "mDNS: " W "%s.local", s_cfg.mdns_host);
}

static void _handle_vendor(const char *param)
{
    if (param == NULL) {
        AT(O "Vendor/hostname: " W "%s", s_cfg.vendor[0] ? s_cfg.vendor : "(nao configurado)");
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: vendor invalido");
        return;
    }
    strncpy(s_cfg.vendor, param, sizeof(s_cfg.vendor) - 1);
    s_cfg.vendor[sizeof(s_cfg.vendor) - 1] = '\0';
    _nvs_set_str("vendor", s_cfg.vendor);
    AT(G "Vendor stored: " W "%s" Y " (aplicado no proximo boot)", s_cfg.vendor);
}

static void _handle_time_status(const char *param)
{
    struct tm local_tm = {0};
    struct tm utc_tm = {0};

    (void)param;

    AT(O "SNTP: " W "%s", s_time_init_done ? "INITIALIZED" : "OFF");
    AT(O "Time synced: " W "%s", esp_network_time_is_synced() ? "TRUE" : "FALSE");
    AT(O "Timezone: " W "%s", s_timezone);

    if (!esp_network_time_is_synced()) {
        return;
    }

    if (esp_network_get_local_time(&local_tm) == ESP_OK) {
        _print_tm_line("Local", &local_tm);
    }
    if (esp_network_get_utc_time(&utc_tm) == ESP_OK) {
        _print_tm_line("UTC", &utc_tm);
    }
}

static void _handle_timezone(const char *param)
{
    esp_err_t err;

    if (param == NULL) {
        AT(O "Timezone: " W "%s", s_timezone);
        return;
    }

    err = esp_network_set_timezone(param);
    if (err != ESP_OK) {
        AT(R "ERROR: timezone invalido");
        return;
    }

    strncpy(s_cfg.tz, s_timezone, sizeof(s_cfg.tz) - 1);
    s_cfg.tz[sizeof(s_cfg.tz) - 1] = '\0';
    _nvs_set_str("tz", s_cfg.tz);
    AT(G "Timezone stored: " W "%s", s_timezone);
}

static void _handle_ntp_status(const char *param)
{
    unsigned int reachability = 0U;
    esp_err_t err;

    (void)param;

    AT(O "NTP: " W "%s", s_time_init_done ? "ON" : "OFF");
    AT(O "Sync: " W "%s", esp_network_time_is_synced() ? "TRUE" : "FALSE");
    AT(O "Server1: " W "pool.ntp.org");
    if (NET_SNTP_SERVER_COUNT > 1) {
        AT(O "Server2: " W "time.google.com");
    }

    if (!s_time_init_done) {
        return;
    }

    err = esp_netif_sntp_reachability(0, &reachability);
    if (err == ESP_OK) {
        AT(O "Reach1: " W "0x%02X", reachability);
    }

    if (NET_SNTP_SERVER_COUNT > 1) {
        err = esp_netif_sntp_reachability(1, &reachability);
        if (err == ESP_OK) {
            AT(O "Reach2: " W "0x%02X", reachability);
        }
    }
}

esp_err_t esp_network_init(void)
{
    const esp_timer_create_args_t reconnect_args = {
        .callback = _reconnect_cb,
        .arg = NULL,
        .name = "net_reconnect",
    };
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_init_done) return ESP_ERR_INVALID_STATE;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, _wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    _nvs_load_all();
    (void)_apply_timezone(s_timezone);

    esp_at_register_cmd_example("AT+WIFI?", _handle_wifi_status, "AT+WIFI? - exibe status completo");
    esp_at_register_cmd_example("AT+WIFI_SSID", _handle_wifi_ssid, "AT+WIFI_SSID=\"MinhaRede\"");
    esp_at_register_cmd_example("AT+WIFI_PWD", _handle_wifi_pwd, "AT+WIFI_PWD=\"sua_senha\"");
    esp_at_register_cmd_example("AT+WIFI_IP", _handle_wifi_ip, "AT+WIFI_IP=\"192.168.1.10\"");
    esp_at_register_cmd_example("AT+WIFI_GW", _handle_wifi_gw, "AT+WIFI_GW=\"192.168.1.1\"");
    esp_at_register_cmd_example("AT+WIFI_SUB", _handle_wifi_sub, "AT+WIFI_SUB=\"255.255.255.0\"");
    esp_at_register_cmd_example("AT+mDNS", _handle_mdns, "AT+mDNS=\"esp32\"");
    esp_at_register_cmd_example("AT+VENDOR", _handle_vendor, "AT+VENDOR=\"sua_empresa\"");
    esp_at_register_cmd_example("AT+TIME?", _handle_time_status, "AT+TIME? - horario local e UTC");
    esp_at_register_cmd_example("AT+TZ", _handle_timezone, "AT+TZ=\"<-03>3\"");
    esp_at_register_cmd_example("AT+NTP?", _handle_ntp_status, "AT+NTP? - status do SNTP");

    s_init_done = true;
    ESP_LOGI(TAG, "inicializado");
    return ESP_OK;
}

esp_err_t esp_network_start(void)
{
    wifi_config_t wcfg = {0};

    if (!s_init_done) return ESP_ERR_INVALID_STATE;
    if (s_cfg.ssid[0] == '\0') {
        AT_W(Y "WiFi: SSID nao configurado - use AT+WIFI_SSID=\"sua_rede\"");
        return ESP_ERR_INVALID_STATE;
    }

    strncpy((char *)wcfg.sta.ssid, s_cfg.ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, s_cfg.pwd, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = (s_cfg.pwd[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    _apply_static_ip();
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t esp_network_time_init(void)
{
    esp_err_t err;
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    if (!s_init_done) return ESP_ERR_INVALID_STATE;
    if (s_time_init_done) return ESP_ERR_INVALID_STATE;

    err = _apply_timezone(s_timezone);
    if (err != ESP_OK) return err;

    err = esp_event_handler_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, _sntp_event_handler, NULL);
    if (err != ESP_OK) return err;

    sntp_cfg.wait_for_sync = true;
    sntp_cfg.start = true;
    sntp_cfg.renew_servers_after_new_IP = true;
    sntp_cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    sntp_cfg.num_of_servers = NET_SNTP_SERVER_COUNT;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    sntp_cfg.servers[1] = "time.google.com";
#endif

    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        esp_event_handler_unregister(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, _sntp_event_handler);
        AT_W(Y "SNTP init falhou: " W "%s", esp_err_to_name(err));
        return err;
    }

    s_time_synced = _time_valid();
    s_time_init_done = true;
    ESP_LOGI(TAG, "time initialized (tz=%s)", s_timezone);
    return ESP_OK;
}

esp_err_t esp_network_time_deinit(void)
{
    if (!s_time_init_done) return ESP_ERR_INVALID_STATE;

    esp_netif_sntp_deinit();
    esp_event_handler_unregister(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, _sntp_event_handler);
    s_time_synced = false;
    s_time_init_done = false;
    return ESP_OK;
}

bool esp_network_time_is_synced(void)
{
    return s_time_synced && _time_valid();
}

esp_err_t esp_network_time_wait_sync(uint32_t timeout_ms)
{
    esp_err_t err;

    if (!s_time_init_done) return ESP_ERR_INVALID_STATE;
    if (esp_network_time_is_synced()) return ESP_OK;

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        s_time_synced = _time_valid();
    }
    return err;
}

esp_err_t esp_network_set_timezone(const char *tz)
{
    return _apply_timezone(tz);
}

esp_err_t esp_network_get_local_time(struct tm *out_time)
{
    time_t now;

    if (out_time == NULL) return ESP_ERR_INVALID_ARG;
    if (!_time_valid()) return ESP_ERR_INVALID_STATE;

    now = time(NULL);
    localtime_r(&now, out_time);
    return ESP_OK;
}

esp_err_t esp_network_get_utc_time(struct tm *out_time)
{
    time_t now;

    if (out_time == NULL) return ESP_ERR_INVALID_ARG;
    if (!_time_valid()) return ESP_ERR_INVALID_STATE;

    now = time(NULL);
    gmtime_r(&now, out_time);
    return ESP_OK;
}
