idf_component_register(SRCS "src/programmer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES common protocol storage arm_adi cortexm_debug
                                mspm0g3507 mspm0_loader mbedtls)
