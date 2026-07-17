idf_component_register(SRCS "src/gateway_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common protocol storage transport_espnow gateway_usb security)

