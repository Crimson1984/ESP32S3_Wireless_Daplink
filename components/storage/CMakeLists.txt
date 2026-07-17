idf_component_register(SRCS "src/datlink_storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common protocol esp_partition mbedtls)
