idf_component_register(SRCS "src/datlink_diagnostics.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common security esp_driver_gpio esp_system spi_flash heap)

