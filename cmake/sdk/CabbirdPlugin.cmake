include(CMakeParseArguments)

function(cabbird_add_plugin target)
    set(options C_ONLY NO_RELEASE)
    set(oneValueArgs MANIFEST PACKAGE_NAME OUTPUT_DIRECTORY)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(CABBIRD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(NOT CABBIRD_SOURCES)
        message(FATAL_ERROR "cabbird_add_plugin(${target}) requires SOURCES")
    endif()
    if(NOT CABBIRD_MANIFEST)
        message(FATAL_ERROR "cabbird_add_plugin(${target}) requires MANIFEST")
    endif()
    if(NOT IS_ABSOLUTE "${CABBIRD_MANIFEST}")
        set(CABBIRD_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${CABBIRD_MANIFEST}")
    endif()
    if(NOT EXISTS "${CABBIRD_MANIFEST}")
        message(FATAL_ERROR "plugin manifest does not exist: ${CABBIRD_MANIFEST}")
    endif()
    if(NOT CABBIRD_PACKAGE_NAME)
        set(CABBIRD_PACKAGE_NAME "${target}")
    endif()
    if(NOT CABBIRD_OUTPUT_DIRECTORY)
        set(CABBIRD_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/${CABBIRD_PACKAGE_NAME}")
    endif()
    add_library(${target} SHARED ${CABBIRD_SOURCES})
    target_link_libraries(${target} PRIVATE Cabbird::sdk)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "plugin"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>"
        PDB_OUTPUT_DIRECTORY "$<1:${CABBIRD_OUTPUT_DIRECTORY}>")
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
        if(NOT CABBIRD_C_ONLY)
            target_compile_options(${target} PRIVATE /EHsc)
        endif()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CABBIRD_MANIFEST}" "$<TARGET_FILE_DIR:${target}>/manifest.json"
        VERBATIM)
    set_property(TARGET ${target} PROPERTY CABBIRD_PACKAGE_DIRECTORY "${CABBIRD_OUTPUT_DIRECTORY}")
    if(NOT CABBIRD_NO_RELEASE)
        set_property(GLOBAL APPEND PROPERTY CABBIRD_RELEASE_PLUGIN_TARGETS "${target}")
    endif()
endfunction()
