/**
 * @file    esp_network.c
 * @brief   Implementação da camada de rede WiFi Station.
 *
 * NVS namespace "net":
 *   ssid   — SSID do AP
 *   pwd    — Senha do AP
 *   ip     — IP estático (vazio = DHCP)
 *   gw     — Gateway (vazio = DHCP)
 *   sub    — Máscara de sub-rede (vazio = DHCP)
 *   mdns   — Hostname mDNS (ex: "fence" → fence.local)
 *   vendor — Hostname na rede local (netif hostname)
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"

#include "esp_at.h"
#include "esp_network.h"

/* ======================================================================== */
/* Configuração                                                              */
/* ======================================================================== */

#define NET_NVS_NAMESPACE   "net"
#define NET_MAX_RETRY       5
#define NET_RECONNECT_US    (3000ULL * 1000ULL)  /**< 3 s em microsegundos (esp_timer) */
#define NET_STR_MAX         64                   /**< Tamanho máximo de strings NVS    */

static const char *TAG = "esp_network";

/* ======================================================================== */
/* Estado interno                                                            */
/* ======================================================================== */

/** Credenciais e parâmetros carregados do NVS na inicialização. */
typedef struct {
    char ssid[NET_STR_MAX];
    char pwd[NET_STR_MAX];
    char ip[NET_STR_MAX];
    char gw[NET_STR_MAX];
    char sub[NET_STR_MAX];
    char mdns_host[NET_STR_MAX];
    char vendor[NET_STR_MAX];
} net_cfg_t;

static net_cfg_t          s_cfg;
static esp_netif_t       *s_netif           = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool               s_init_done       = false;
static int                s_retry           = 0;

/* ======================================================================== */
/* Helpers NVS                                                               */
/* ======================================================================== */

/**
 * @brief  Lê uma string do NVS, namespace "net".
 *
 * @param[in]  key  Chave NVS.
 * @param[out] dst  Buffer de destino.
 * @param[in]  len  Tamanho do buffer.
 * @return ESP_OK se encontrado, ESP_ERR_NVS_NOT_FOUND se ausente.
 */
static esp_err_t _nvs_get_str(const char *key, char *dst, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_str(h, key, dst, &len);
    nvs_close(h);
    return err;
}

/**
 * @brief  Grava uma string no NVS, namespace "net", e faz commit.
 *
 * @param[in] key  Chave NVS.
 * @param[in] val  String a gravar.
 * @return ESP_OK em sucesso.
 */
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

/**
 * @brief  Carrega todas as chaves da namespace "net" para s_cfg.
 *         Campos ausentes ficam com string vazia.
 */
static void _nvs_load_all(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    size_t len;

    len = sizeof(s_cfg.ssid);       _nvs_get_str("ssid",   s_cfg.ssid,       len);
    len = sizeof(s_cfg.pwd);        _nvs_get_str("pwd",    s_cfg.pwd,        len);
    len = sizeof(s_cfg.ip);         _nvs_get_str("ip",     s_cfg.ip,         len);
    len = sizeof(s_cfg.gw);         _nvs_get_str("gw",     s_cfg.gw,         len);
    len = sizeof(s_cfg.sub);        _nvs_get_str("sub",    s_cfg.sub,        len);
    len = sizeof(s_cfg.mdns_host);  _nvs_get_str("mdns",   s_cfg.mdns_host,  len);
    len = sizeof(s_cfg.vendor);     _nvs_get_str("vendor", s_cfg.vendor,     len);
}

/* ======================================================================== */
/* mDNS                                                                     */
/* ======================================================================== */

/**
 * @brief  (Re)inicia o serviço mDNS com o hostname atual de s_cfg.
 *         Se o hostname estiver vazio, usa "esp32" como padrão.
 */
static void _start_mdns(void)
{
    const char *host = (s_cfg.mdns_host[0] != '\0') ? s_cfg.mdns_host : "esp32";

    mdns_free();
    if (mdns_init() != ESP_OK) {
        AT_E(R "mDNS: falha ao inicializar");
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set(DEVICE_NAME);
    AT_I(G "mDNS: " W "%s.local", host);
}

/* ======================================================================== */
/* Configuração de IP (estático ou DHCP)                                    */
/* ======================================================================== */

/**
 * @brief  Aplica IP estático se ip/gw/sub estiverem todos preenchidos.
 *         Caso contrário não faz nada (DHCP permanece ativo).
 */
static void _apply_static_ip(void)
{
    if (s_cfg.ip[0] == '\0' || s_cfg.gw[0] == '\0' || s_cfg.sub[0] == '\0') {
        return; /* DHCP */
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(s_cfg.ip,  &ip_info.ip)      != ESP_OK ||
        esp_netif_str_to_ip4(s_cfg.gw,  &ip_info.gw)      != ESP_OK ||
        esp_netif_str_to_ip4(s_cfg.sub, &ip_info.netmask)  != ESP_OK) {
        AT_W(Y "IP estático: formato inválido — usando DHCP");
        return;
    }

    esp_netif_dhcpc_stop(s_netif);
    if (esp_netif_set_ip_info(s_netif, &ip_info) != ESP_OK) {
        AT_W(Y "IP estático: falha ao configurar — usando DHCP");
        esp_netif_dhcpc_start(s_netif);
    }
}

/* ======================================================================== */
/* Timer de reconexão                                                        */
/* ======================================================================== */

/**
 * @brief  Callback do timer de reconexão WiFi.
 *
 * Chamado pelo esp_timer após NET_RECONNECT_US sem conexão.
 * Executa na timer task — não bloqueia o event loop.
 *
 * @param[in] arg  Argumento de contexto (não utilizado).
 */
static void _reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* ======================================================================== */
/* Event handler WiFi / IP                                                   */
/* ======================================================================== */

/**
 * @brief  Handler unificado para eventos WiFi e IP.
 *
 * Mantido estritamente não-bloqueante: nenhuma chamada a vTaskDelay,
 * mutex ou operação de I/O longa.  A reconexão com atraso é delegada
 * ao timer @ref s_reconnect_timer para não impedir o processamento de
 * outros eventos no event loop padrão.
 *
 * @param[in] arg        Argumento de contexto (não utilizado).
 * @param[in] event_base Base do evento (WIFI_EVENT ou IP_EVENT).
 * @param[in] event_id   ID do evento.
 * @param[in] event_data Dados do evento.
 */
static void _wifi_event_handler(void            *arg,
                                esp_event_base_t event_base,
                                int32_t          event_id,
                                void            *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {

        case WIFI_EVENT_STA_START:
            s_retry = 0;
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            /* Cancela retry pendente (race: timer disparado antes de conectar) */
            esp_timer_stop(s_reconnect_timer);

            wifi_event_sta_connected_t *ev = event_data;
            char ssid[33] = {0};
            memcpy(ssid, ev->ssid, ev->ssid_len);
            AT_I(G "WiFi conectado — AP: " W "%s", ssid);

            if (s_cfg.vendor[0] != '\0') {
                esp_netif_set_hostname(s_netif, s_cfg.vendor);
            }
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = event_data;
            if (s_retry < NET_MAX_RETRY) {
                s_retry++;
                AT_W(Y "WiFi desconectado (reason %d) — tentativa %d/%d",
                     ev->reason, s_retry, NET_MAX_RETRY);
                /* Agenda reconexão sem bloquear o event loop */
                esp_timer_start_once(s_reconnect_timer, NET_RECONNECT_US);
            } else {
                AT_E(R "WiFi: máximo de tentativas atingido — desistindo");
            }
            break;
        }

        default:
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = event_data;
        char ip_str[16], gw_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ev->ip_info.gw));

        s_retry = 0;
        AT_I(G "IP: " W "%s  " G "GW: " W "%s", ip_str, gw_str);

        _start_mdns();
    }
}

/* ======================================================================== */
/* Handlers AT                                                               */
/* ======================================================================== */

/** AT+WIFI? — exibe status completo */
static void _handle_wifi_status(const char *param)
{
    (void)param;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        AT(O "WiFi: " R "DISCONNECTED");
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_netif, &ip_info);

    esp_netif_dns_info_t dns_main, dns_backup;
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN,   &dns_main);
    esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);

    AT(O "WiFi: " G "CONNECTED");
    at(O "  SSID   : " W); AT(O "%s",   (char *)ap.ssid);
    at(O "  RSSI   : " W); AT(C "%d dBm", ap.rssi);
    at(O "  IP     : " W); AT(W IPSTR,  IP2STR(&ip_info.ip));
    at(O "  GW     : " W); AT(W IPSTR,  IP2STR(&ip_info.gw));
    at(O "  Subnet : " W); AT(W IPSTR,  IP2STR(&ip_info.netmask));
    at(O "  DNS1   : " W); AT(W IPSTR,  IP2STR(&dns_main.ip.u_addr.ip4));
    at(O "  DNS2   : " W); AT(W IPSTR,  IP2STR(&dns_backup.ip.u_addr.ip4));
    if (s_cfg.mdns_host[0] != '\0') {
        at(O "  mDNS   : " W); AT(C "%s.local", s_cfg.mdns_host);
    }
}

/** AT+WIFI_SSID[="value"] */
static void _handle_wifi_ssid(const char *param)
{
    if (param == NULL) {
        if (s_cfg.ssid[0] != '\0')
            AT(O "SSID: " W "%s", s_cfg.ssid);
        else
            AT(Y "SSID: " R "(não configurado)");
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: SSID inválido");
        return;
    }
    strncpy(s_cfg.ssid, param, sizeof(s_cfg.ssid) - 1);
    s_cfg.ssid[sizeof(s_cfg.ssid) - 1] = '\0';
    _nvs_set_str("ssid", s_cfg.ssid);
    AT(G "SSID stored: " W "%s", s_cfg.ssid);
}

/** AT+WIFI_PWD[="value"] */
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

/** AT+WIFI_IP[="x.x.x.x"] */
static void _handle_wifi_ip(const char *param)
{
    if (param == NULL) {
        AT(O "IP estático: " W "%s", (s_cfg.ip[0] ? s_cfg.ip : "(DHCP)"));
        return;
    }
    strncpy(s_cfg.ip, param, sizeof(s_cfg.ip) - 1);
    s_cfg.ip[sizeof(s_cfg.ip) - 1] = '\0';
    _nvs_set_str("ip", s_cfg.ip);
    AT(G "IP stored: " W "%s", s_cfg.ip);
}

/** AT+WIFI_GW[="x.x.x.x"] */
static void _handle_wifi_gw(const char *param)
{
    if (param == NULL) {
        AT(O "GW: " W "%s", (s_cfg.gw[0] ? s_cfg.gw : "(DHCP)"));
        return;
    }
    strncpy(s_cfg.gw, param, sizeof(s_cfg.gw) - 1);
    s_cfg.gw[sizeof(s_cfg.gw) - 1] = '\0';
    _nvs_set_str("gw", s_cfg.gw);
    AT(G "GW stored: " W "%s", s_cfg.gw);
}

/** AT+WIFI_SUB[="x.x.x.x"] */
static void _handle_wifi_sub(const char *param)
{
    if (param == NULL) {
        AT(O "Subnet: " W "%s", (s_cfg.sub[0] ? s_cfg.sub : "(DHCP)"));
        return;
    }
    strncpy(s_cfg.sub, param, sizeof(s_cfg.sub) - 1);
    s_cfg.sub[sizeof(s_cfg.sub) - 1] = '\0';
    _nvs_set_str("sub", s_cfg.sub);
    AT(G "Subnet stored: " W "%s", s_cfg.sub);
}

/** AT+mDNS[="hostname"] */
static void _handle_mdns(const char *param)
{
    if (param == NULL) {
        AT(O "mDNS: " W "%s",
           (s_cfg.mdns_host[0] ? s_cfg.mdns_host : "(não configurado)"));
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: hostname mDNS inválido");
        return;
    }
    strncpy(s_cfg.mdns_host, param, sizeof(s_cfg.mdns_host) - 1);
    s_cfg.mdns_host[sizeof(s_cfg.mdns_host) - 1] = '\0';
    _nvs_set_str("mdns", s_cfg.mdns_host);
    _start_mdns();
    AT(G "mDNS: " W "%s.local", s_cfg.mdns_host);
}

/** AT+VENDOR[="name"] */
static void _handle_vendor(const char *param)
{
    if (param == NULL) {
        AT(O "Vendor/hostname: " W "%s",
           (s_cfg.vendor[0] ? s_cfg.vendor : "(não configurado)"));
        return;
    }
    if (strlen(param) == 0 || strlen(param) >= NET_STR_MAX) {
        AT(R "ERROR: vendor inválido");
        return;
    }
    strncpy(s_cfg.vendor, param, sizeof(s_cfg.vendor) - 1);
    s_cfg.vendor[sizeof(s_cfg.vendor) - 1] = '\0';
    _nvs_set_str("vendor", s_cfg.vendor);
    AT(G "Vendor stored: " W "%s" Y " (aplicado no próximo boot)", s_cfg.vendor);
}

/* ======================================================================== */
/* API pública                                                               */
/* ======================================================================== */

esp_err_t esp_network_init(void)
{
    if (s_init_done) return ESP_ERR_INVALID_STATE;

    /* 1. TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* 2. Event loop padrão */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Interface netif STA */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) return ESP_FAIL;

    /* 4. Timer de reconexão — criado uma vez, reutilizado a cada desconexão */
    const esp_timer_create_args_t reconnect_args = {
        .callback = _reconnect_cb,
        .arg      = NULL,
        .name     = "net_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_args, &s_reconnect_timer));

    /* 5. Event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,    _wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,   IP_EVENT_STA_GOT_IP, _wifi_event_handler, NULL));

    /* 6. Driver WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* 7. Carregar NVS */
    _nvs_load_all();

    /* 8. Registrar comandos AT */
    esp_at_register_cmd("AT+WIFI?",     _handle_wifi_status);
    esp_at_register_cmd("AT+WIFI_SSID", _handle_wifi_ssid);
    esp_at_register_cmd("AT+WIFI_PWD",  _handle_wifi_pwd);
    esp_at_register_cmd("AT+WIFI_IP",   _handle_wifi_ip);
    esp_at_register_cmd("AT+WIFI_GW",   _handle_wifi_gw);
    esp_at_register_cmd("AT+WIFI_SUB",  _handle_wifi_sub);
    esp_at_register_cmd("AT+mDNS",      _handle_mdns);
    esp_at_register_cmd("AT+VENDOR",    _handle_vendor);

    s_init_done = true;
    ESP_LOGI(TAG, "inicializado");
    return ESP_OK;
}

esp_err_t esp_network_start(void)
{
    if (!s_init_done) return ESP_ERR_INVALID_STATE;

    if (s_cfg.ssid[0] == '\0') {
        AT_W(Y "WiFi: SSID não configurado — use AT+WIFI_SSID=\"sua_rede\"");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configura credenciais */
    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid,     s_cfg.ssid, sizeof(wcfg.sta.ssid)     - 1);
    strncpy((char *)wcfg.sta.password, s_cfg.pwd,  sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = (s_cfg.pwd[0] != '\0')
                                  ? WIFI_AUTH_WPA2_PSK
                                  : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));

    /* Aplica IP estático antes de subir o link, se configurado */
    _apply_static_ip();

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}
