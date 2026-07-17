idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES diagnostics security storage transport_espnow nvs_flash
                                gateway_app probe_app)
