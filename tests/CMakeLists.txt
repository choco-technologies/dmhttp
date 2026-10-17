set(DMOD_MODULE_NAME        test_dmhttp)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         16384)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmhttp_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmhttp_if)
dmhttp_audit_module(${DMOD_MODULE_NAME})
dmod_link_modules(${DMOD_MODULE_NAME} dmtls)

# Register the test module with ctest - this replays the same two steps CI
# runs (see .github/templates/ci.yml.template): install the test module's
# dependencies with dmf-get, then execute it through dmod_loader. The test's
# exit code equals its number of failed steps (see dmod_add_test).
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

set(test_manifest_env "")
if(DEFINED DMOD_DMM_URL AND NOT "${DMOD_DMM_URL}" STREQUAL "")
    list(APPEND test_manifest_env "DMOD_MANIFEST=${DMOD_DMM_URL}")
endif()
if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            ${test_manifest_env}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()

if(NOT CMAKE_CROSSCOMPILING AND NOT DMOD_ARCH MATCHES "arm")
    add_subdirectory(host)
endif()
option(DMHTTP_BUILD_BOARD_PROBE "Build real TCP hardware diagnostic" OFF)
if(DMHTTP_BUILD_BOARD_PROBE)
    add_subdirectory(probe)
endif()
