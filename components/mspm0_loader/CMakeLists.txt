if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated/mspm0_loader_blob.c")
    message(FATAL_ERROR "MSPM0 loader blob is missing; run tools/mspm0_loader/build_loader.ps1")
endif()

idf_component_register(SRCS "src/mspm0_loader.c" "generated/mspm0_loader_blob.c"
                       INCLUDE_DIRS "include" "generated"
                       REQUIRES common arm_adi cortexm_debug)
