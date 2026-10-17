set(DMOD_MODULE_NAME httpprobe)
# Diagnostics do crypto work: keep them above the FreeRTOS idle priority.
set(DMOD_PRIORITY 1)
set(DMOD_STACK_SIZE 16384)
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/httpprobe.dmr)
dmod_add_executable(${DMOD_MODULE_NAME} ${PROJECT_VERSION} httpprobe.c)
target_link_libraries(${DMOD_MODULE_NAME} dmhttp_if)
dmod_link_modules(${DMOD_MODULE_NAME} dmtls dmtcp dmdns dmip dmroute dmnetif dmrng dmosi)

set(DMOD_MODULE_NAME httpwireprobe)
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/httpprobe.dmr)
dmod_add_executable(${DMOD_MODULE_NAME} ${PROJECT_VERSION} wireprobe.c)
target_link_libraries(${DMOD_MODULE_NAME} dmhttp_if)
dmod_link_modules(${DMOD_MODULE_NAME} dmtls dmrng dmosi dmtty)
