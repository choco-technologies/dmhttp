find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(dmhttp_audit_module target)
    # Metadata is checked at build time and excluded from the binary by objcopy.
    target_link_options(${target} PRIVATE -Wl,--emit-relocs,--no-relax)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        ${PROJECT_SOURCE_DIR}/scripts/check_dmod_pointers.py)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_dmod_pointers.py
            $<TARGET_FILE:${target}> ${DMOD_DMF_DIR}/${target}.dmf
        COMMENT "Checking compatibility with the existing DMOD GOT loader" VERBATIM)
endfunction()
