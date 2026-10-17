find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
include(FetchContent)
FetchContent_Declare(dmhttp_tls GIT_REPOSITORY https://github.com/choco-technologies/dmtls.git GIT_TAG main GIT_SHALLOW TRUE)
FetchContent_GetProperties(dmhttp_tls)
if(NOT dmhttp_tls_POPULATED)
    FetchContent_Populate(dmhttp_tls)
endif()
include(${dmhttp_tls_SOURCE_DIR}/cmake/backend.cmake)
set(tls_sources ${dmhttp_tls_SOURCE_DIR}/src/dmtls.c ${dmhttp_tls_SOURCE_DIR}/src/dmtls_platform.c
    ${dmhttp_tls_SOURCE_DIR}/src/dmtls_config.c ${dmhttp_tls_SOURCE_DIR}/src/dmtls_connection.c ${dmhttp_tls_SOURCE_DIR}/src/dmtls_io.c)
set_source_files_properties(${dmhttp_tls_SOURCE_DIR}/src/dmtls.c PROPERTIES COMPILE_DEFINITIONS "dmod_init=dmtls_test_module_init;dmod_deinit=dmtls_test_module_deinit")
add_library(dmhttp_host_engine SHARED ../../src/dmhttp.c ../../src/dmhttp_url.c ../../src/dmhttp_parser.c
    platform.c ${tls_sources} ${DMTLS_BACKEND_SOURCES})
target_include_directories(dmhttp_host_engine PRIVATE stubs ../../include ../../src
    ${dmhttp_tls_SOURCE_DIR}/include ${dmhttp_tls_SOURCE_DIR}/src ${dmhttp_tls_SOURCE_DIR}/config
    ${DMOD_DMF_DIR}/inc/dmtls/include ${DMTLS_BACKEND_INCLUDES})
target_compile_definitions(dmhttp_host_engine PRIVATE strstr=dmtls_backend_strstr
    MBEDTLS_CONFIG_FILE="dmtls_mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="dmtls_psa_config.h")
target_link_libraries(dmhttp_host_engine PRIVATE Threads::Threads)
add_test(NAME http_interoperability COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/interoperability.py $<TARGET_FILE:dmhttp_host_engine>)
set_tests_properties(http_interoperability PROPERTIES TIMEOUT 180)
