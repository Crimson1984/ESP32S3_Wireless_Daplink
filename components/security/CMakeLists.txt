if(NOT DEFINED ENV{DATLINK_GENERATED_DIR})
    message(FATAL_ERROR "DATLINK_GENERATED_DIR is not set; use build.ps1")
endif()

idf_component_register(SRCS "src/datlink_security.c"
                       INCLUDE_DIRS "include" "$ENV{DATLINK_GENERATED_DIR}"
                       REQUIRES common esp_wifi)

