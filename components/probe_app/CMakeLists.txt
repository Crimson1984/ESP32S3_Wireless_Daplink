idf_component_register(SRCS "src/probe_app.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common protocol storage transport_espnow programmer)

