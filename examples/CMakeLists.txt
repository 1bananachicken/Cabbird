cmake_minimum_required(VERSION 3.22)

project(CabbirdSDKExamples LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Cabbird SDK examples target Windows x64")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CabbirdSDK CONFIG REQUIRED)

set(CABBIRD_EXAMPLE_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/packages")

cabbird_add_plugin(cabbird_example_hello_ui C_ONLY
    SOURCES hello_ui/plugin.c
    MANIFEST hello_ui/manifest.json
    PACKAGE_NAME HelloUi
    OUTPUT_DIRECTORY "${CABBIRD_EXAMPLE_OUTPUT_ROOT}/HelloUi")

cabbird_add_plugin(cabbird_example_tick_counter
    SOURCES tick_counter/plugin.cpp
    MANIFEST tick_counter/manifest.json
    PACKAGE_NAME TickCounter
    OUTPUT_DIRECTORY "${CABBIRD_EXAMPLE_OUTPUT_ROOT}/TickCounter")

cabbird_add_plugin(cabbird_example_reliable_config
    SOURCES reliable_config/plugin.cpp
    MANIFEST reliable_config/manifest.json
    PACKAGE_NAME ReliableConfig
    OUTPUT_DIRECTORY "${CABBIRD_EXAMPLE_OUTPUT_ROOT}/ReliableConfig")

cabbird_add_plugin(cabbird_example_unity_entity_inspector
    SOURCES unity_entity_inspector/plugin.cpp
    MANIFEST unity_entity_inspector/manifest.json
    PACKAGE_NAME UnityEntityInspector
    OUTPUT_DIRECTORY "${CABBIRD_EXAMPLE_OUTPUT_ROOT}/UnityEntityInspector")

# Two examples were dropped from the upstream set: nte_inspector and nte_combat_demo.
# They read NTE's own combat/menu state through NTE services, so they are NTE-specific
# content rather than framework examples, and nothing NTE-specific is ported here.  The three
# targets kept below exercise the ABI itself: a C-only
# plugin, the scheduler/tick subscription, and the config-watch path.
#
# A third upstream example was replaced rather than dropped: `unity_world_inspector` queried
# `cabbird.unity.build` / `.objects` / `.world`, three UE5 service declarations that nothing in
# this repository ever published.  It is now `unity_entity_inspector`, built on the entity
# source the host does publish.
foreach(example_target IN ITEMS cabbird_example_hello_ui cabbird_example_tick_counter
        cabbird_example_reliable_config cabbird_example_unity_entity_inspector)
    target_compile_definitions(
        ${example_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endforeach()