idf_component_register(SRCS "main.c" INCLUDE_DIRS "." EMBED_TXTFILES "page.html"
                       REQUIRES closer_demo esp_http_server nvs_flash driver esp_driver_gpio cjson esp_timer lwip esp_wifi)
