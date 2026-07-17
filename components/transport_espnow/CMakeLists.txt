idf_component_register(SRCS "src/datlink_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common protocol security diagnostics esp_wifi esp_event esp_netif nvs_flash)
