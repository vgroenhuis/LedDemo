idf_component_register(
    SRCS "closer_demo.c" "closer_ble.c" "closer_wifi_web.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "wifi.html"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_client esp_https_ota app_update esp_timer cjson mbedtls esp-tls bt esp_http_server)
string(TIMESTAMP CLOSER_BUILD_EPOCH "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CLOSER_BUILD_EPOCH=${CLOSER_BUILD_EPOCH})
