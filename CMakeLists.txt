cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(DEFINED ENV{DATLINK_ROLE_NAME})
    set(DATLINK_PROJECT_NAME "datlink_$ENV{DATLINK_ROLE_NAME}")
else()
    set(DATLINK_PROJECT_NAME "wireless_datlink")
endif()

project(${DATLINK_PROJECT_NAME})

