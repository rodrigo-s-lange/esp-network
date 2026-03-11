/**
 * @file    esp_network.h
 * @brief   Camada de rede WiFi Station com persistência NVS e mDNS.
 *
 * Este componente gerencia a conexão WiFi Station do ESP32, permitindo
 * configuração em tempo de execução via comandos AT e persistência das
 * credenciais e parâmetros de rede no NVS (namespace "net").
 *
 * @par Dependência obrigatória
 *   Requer o componente **rodrigo-s-lange/esp_at**.
 *   esp_at_init() DEVE ser chamado antes de esp_network_init().
 *   Repositório: https://github.com/rodrigo-s-lange/esp_at
 *
 * Fluxo de uso:
 * @code
 *   nvs_flash_init();
 *   esp_at_init();
 *   esp_network_init();
 *   esp_network_start();
 * @endcode
 *
 * Comandos AT registrados automaticamente por esp_network_init():
 * @code
 *   AT+WIFI?               → status completo (IP, RSSI, GW, DNS)
 *   AT+WIFI_SSID="MinhaRede" → salva SSID; sem '=' → exibe atual
 *   AT+WIFI_PWD="senha"    → salva senha; sem '=' → exibe "***"
 *   AT+WIFI_IP="192.168.1.10"  → IP estático; sem '=' → exibe atual
 *   AT+WIFI_GW="192.168.1.1"   → gateway;   sem '=' → exibe atual
 *   AT+WIFI_SUB="255.255.255.0" → máscara;  sem '=' → exibe atual
 *   AT+mDNS="fence"        → aplica mDNS imediato + salva NVS
 *   AT+VENDOR="SawFence"   → hostname de rede (aplicado no próximo boot)
 * @endcode
 *
 * Lógica IP estático vs DHCP:
 *   Se "ip", "gw" e "sub" estiverem todos preenchidos no NVS →
 *   configura IP estático antes de conectar; caso contrário → DHCP.
 *
 * Auto-reconexão: até 5 tentativas com intervalo de 3 s (via esp_timer,
 * não-bloqueante); após isso para de tentar até novo AT+WIFI_SSID.
 *
 * @note Thread-safe: todos os handlers AT são executados dentro da
 *       at_task; operações NVS são atômicas por nvs_handle.
 *
 * @par Registry
 *   rodrigo-s-lange/esp_network
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================== */
/* Inicialização                                                             */
/* ======================================================================== */

/**
 * @brief  Inicializa o componente de rede.
 *
 * Realiza as seguintes operações na ordem:
 *  1. Inicializa o TCP/IP stack (esp_netif_init).
 *  2. Cria o event loop padrão (esp_event_loop_create_default).
 *  3. Cria a interface netif para WiFi Station.
 *  4. Cria o timer de reconexão (esp_timer — não-bloqueante).
 *  5. Registra os event handlers para WIFI_EVENT e IP_EVENT.
 *  6. Configura e inicializa o driver WiFi (WIFI_INIT_CONFIG_DEFAULT).
 *  7. Lê credenciais e parâmetros salvos no NVS (namespace "net").
 *  8. Registra os 8 comandos AT do componente via esp_at_register_cmd().
 *
 * Deve ser chamada após esp_at_init() e nvs_flash_init().
 * Não inicia a conexão — use esp_network_start() para isso.
 *
 * @return ESP_OK                Inicialização bem-sucedida.
 * @return ESP_ERR_INVALID_STATE Já inicializado.
 * @return Outros                Erros de netif, event loop ou WiFi.
 */
esp_err_t esp_network_init(void);

/* ======================================================================== */
/* Conexão                                                                   */
/* ======================================================================== */

/**
 * @brief  Inicia a conexão WiFi Station.
 *
 * Chama esp_wifi_start(), que dispara WIFI_EVENT_STA_START e, em
 * seguida, esp_wifi_connect() automaticamente dentro do event handler.
 * Se nenhum SSID estiver configurado no NVS, registra aviso e retorna
 * ESP_ERR_INVALID_STATE sem iniciar o WiFi.
 *
 * @pre esp_network_init() deve ter sido chamado com sucesso.
 *
 * @return ESP_OK                WiFi iniciado (conexão assíncrona).
 * @return ESP_ERR_INVALID_STATE SSID não configurado ou não inicializado.
 * @return Outros                Erros da camada esp_wifi.
 */
esp_err_t esp_network_start(void);

#ifdef __cplusplus
}
#endif
