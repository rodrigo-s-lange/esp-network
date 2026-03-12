/**
 * @file    esp_webterm.c
 * @brief   Servidor WebTerm (HTTP + WebSocket) para o terminal AT.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "esp_at.h"
#include "esp_network.h"

#define WEBTERM_MAX_CLIENTS       4
#define WEBTERM_MAX_RX            256

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static const char *TAG = "esp_webterm";

static httpd_handle_t    s_server       = NULL;
static SemaphoreHandle_t s_clients_mtx  = NULL;
static int               s_clients[WEBTERM_MAX_CLIENTS] = { -1, -1, -1, -1 };

typedef struct {
    size_t len;
    char   data[];
} ws_broadcast_work_t;

static void _remove_client(int fd);

static bool _has_client(int fd)
{
    bool found = false;
    if (s_clients_mtx == NULL) return false;

    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < WEBTERM_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_clients_mtx);
    return found;
}

static esp_err_t _send_ws_frame_fd(int fd, const char *data, size_t len)
{
    if (s_server == NULL || data == NULL || len == 0) return ESP_ERR_INVALID_STATE;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
    };

    /*
     * This function is called from work queued by httpd_queue_work(),
     * so we are already in HTTP server context. Using httpd_ws_send_data()
     * here would queue another work item and block waiting for completion.
     * Use the async low-level send directly to avoid deadlock.
     */
    return httpd_ws_send_frame_async(s_server, fd, &frame);
}

static esp_err_t _send_ws_frame_req(httpd_req_t *req, const char *data, size_t len)
{
    if (req == NULL || data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
    };

    return httpd_ws_send_frame(req, &frame);
}

static void _ws_broadcast_work(void *arg)
{
    ws_broadcast_work_t *work = (ws_broadcast_work_t *)arg;
    if (work == NULL || work->len == 0) {
        free(work);
        return;
    }

    int targets[WEBTERM_MAX_CLIENTS];
    int count = 0;

    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < WEBTERM_MAX_CLIENTS; i++) {
        if (s_clients[i] >= 0) {
            targets[count++] = s_clients[i];
        }
    }
    xSemaphoreGive(s_clients_mtx);

    for (int i = 0; i < count; i++) {
        int fd = targets[i];
        if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            _remove_client(fd);
            continue;
        }
        if (_send_ws_frame_fd(fd, work->data, work->len) != ESP_OK) {
            ESP_LOGW(TAG, "falha ao enviar WS para fd=%d", fd);
            _remove_client(fd);
        }
    }

    free(work);
}

static void _at_output_sink(const char *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_server == NULL || data == NULL || len == 0 || s_clients_mtx == NULL) return;

    ws_broadcast_work_t *work = malloc(sizeof(*work) + len);
    if (work == NULL) return;

    work->len = len;
    memcpy(work->data, data, len);

    esp_err_t err = httpd_queue_work(s_server, _ws_broadcast_work, work);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_queue_work falhou: %s", esp_err_to_name(err));
        free(work);
    }
}

static esp_err_t _http_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static esp_err_t _add_client(int fd)
{
    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < WEBTERM_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            xSemaphoreGive(s_clients_mtx);
            return ESP_OK;
        }
    }
    for (int i = 0; i < WEBTERM_MAX_CLIENTS; i++) {
        if (s_clients[i] < 0) {
            s_clients[i] = fd;
            xSemaphoreGive(s_clients_mtx);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_clients_mtx);
    return ESP_ERR_NO_MEM;
}

static void _remove_client(int fd)
{
    if (s_clients_mtx == NULL) return;

    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < WEBTERM_MAX_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
            break;
        }
    }
    xSemaphoreGive(s_clients_mtx);
}

static esp_err_t _ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    /* Garante que qualquer socket WS ativo esteja registrado para broadcast */
    if (!_has_client(fd)) {
        esp_err_t err = _add_client(fd);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "ws conectado fd=%d", fd);
    }

    /* Handshake GET nao carrega frame de dados */
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
    };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv header falhou fd=%d", fd);
        _remove_client(fd);
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "ws fechado fd=%d", fd);
        _remove_client(fd);
        return ESP_OK;
    }

    char *payload = NULL;
    if (frame.len > 0) {
        if (frame.len >= WEBTERM_MAX_RX) {
            /* drena o payload para não corromper o estado do socket */
            uint8_t *discard = malloc(frame.len);
            if (discard) {
                frame.payload = discard;
                httpd_ws_recv_frame(req, &frame, frame.len);
                free(discard);
                const char *msg = "ERROR: comando muito longo\r\n";
                _send_ws_frame_req(req, msg, strlen(msg));
                return ESP_OK;
            }
            /* sem memória para drenar: encerra a conexão */
            _remove_client(fd);
            return ESP_FAIL;
        }

        payload = calloc(1, frame.len + 1);
        if (payload == NULL) return ESP_ERR_NO_MEM;

        frame.payload = (uint8_t *)payload;
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) {
            free(payload);
            ESP_LOGW(TAG, "ws recv payload falhou fd=%d", fd);
            _remove_client(fd);
            return err;
        }
        payload[frame.len] = '\0';
    }

    if (frame.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_PONG,
            .payload = (uint8_t *)payload,
            .len = frame.len,
        };
        httpd_ws_send_frame(req, &pong);
        free(payload);
        return ESP_OK;
    }

    if (frame.type == HTTPD_WS_TYPE_PONG) {
        free(payload);
        return ESP_OK;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) {
        free(payload);
        return ESP_OK;
    }

    while (frame.len > 0 && (payload[frame.len - 1] == '\r' || payload[frame.len - 1] == '\n')) {
        payload[--frame.len] = '\0';
    }

    if (frame.len > 0) {
        esp_err_t feed_err = esp_at_feed_line(payload);
        if (feed_err != ESP_OK) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ERROR: feed failed (%s)\r\n", esp_err_to_name(feed_err));
            _send_ws_frame_req(req, msg, strlen(msg));
        }
    }

    free(payload);
    return ESP_OK;
}

esp_err_t esp_webterm_init(uint16_t port)
{
    if (port == 0) return ESP_ERR_INVALID_ARG;
    if (s_server != NULL) return ESP_ERR_INVALID_STATE;

    if (s_clients_mtx == NULL) {
        s_clients_mtx = xSemaphoreCreateMutex();
        if (s_clients_mtx == NULL) return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = _http_index_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = _ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };

    err = httpd_register_uri_handler(s_server, &index_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = httpd_register_uri_handler(s_server, &ws_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    err = esp_at_register_output_sink(_at_output_sink, NULL);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WebTerm ativo em http://<esp-ip>:%u", (unsigned)port);
    return ESP_OK;
}
