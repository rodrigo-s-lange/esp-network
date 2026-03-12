idf_component_register(SRCS "esp_network.c" "esp_webterm.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_at esp_wifi esp_netif esp_event nvs_flash mdns
                             esp_timer esp_system freertos
                             esp_http_server
                    EMBED_TXTFILES "web/index.html")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FIRMWARE_VERSION="v0.1.0"
)
