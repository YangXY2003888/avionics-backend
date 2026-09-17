cmake_minimum_required(VERSION 3.20)
project(avionics_backend VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_package(Threads REQUIRED)

add_library(project_options INTERFACE)
target_include_directories(project_options INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_options(project_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)

add_library(bus_archive STATIC src/storage/archive.cpp)
target_link_libraries(bus_archive PUBLIC project_options)

add_library(bus_core STATIC
    src/platform/shared_library.cpp
    src/plugin/plugin_manager.cpp
    src/bus/bus_manager.cpp
    src/pipeline/frame_pipeline.cpp
    src/decoder/simulated_decoder.cpp
    src/decoder/configuration.cpp
    src/decoder/dictionary_decoder.cpp
    src/analysis/processing.cpp
    src/storage/jsonl_analysis.cpp
    src/analysis/rule_analyzer.cpp
    src/protocol/protocols.cpp
    src/protocol/protocol_router.cpp
    src/protocol/protocol_pipeline.cpp
    src/export/flat_export.cpp)
target_link_libraries(bus_core PUBLIC bus_archive project_options Threads::Threads ${CMAKE_DL_LIBS})

function(add_bus_plugin name source)
    add_library(${name} MODULE ${source})
    target_link_libraries(${name} PRIVATE project_options Threads::Threads)
    target_compile_definitions(${name} PRIVATE BUS_PLUGIN_BUILD)
    set_target_properties(${name} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
endfunction()
add_bus_plugin(bus_simulated plugins/simulated/simulated_plugin.cpp)
add_bus_plugin(bus_replay plugins/replay/replay_plugin.cpp)
add_bus_plugin(bus_random plugins/random/random_plugin.cpp)
target_link_libraries(bus_replay PRIVATE bus_archive)

add_executable(bus_backend src/main.cpp)
target_link_libraries(bus_backend PRIVATE bus_core)
add_executable(bus_analyze src/analyze_main.cpp)
target_link_libraries(bus_analyze PRIVATE bus_core)
add_executable(bus_protocol src/protocol/protocol_main.cpp)
target_link_libraries(bus_protocol PRIVATE bus_core)
add_executable(bus_export src/export/export_main.cpp)
target_link_libraries(bus_export PRIVATE bus_core)

include(CTest)
if(BUILD_TESTING)
    add_bus_plugin(bus_incompatible tests/incompatible_plugin.cpp)
    add_library(c_header_check OBJECT tests/c_header_check.c)
    target_link_libraries(c_header_check PRIVATE project_options)
    add_executable(bus_tests tests/integration.cpp)
    target_link_libraries(bus_tests PRIVATE bus_core)
    add_test(NAME backend_integration COMMAND bus_tests
        $<TARGET_FILE:bus_simulated> $<TARGET_FILE:bus_replay>
        $<TARGET_FILE:bus_incompatible> "${CMAKE_BINARY_DIR}/test-output")
    set_tests_properties(backend_integration PROPERTIES TIMEOUT 30)
    add_executable(processing_tests tests/processing.cpp)
    target_link_libraries(processing_tests PRIVATE bus_core)
    add_test(NAME processing_integration COMMAND processing_tests
        "${PROJECT_SOURCE_DIR}/config/correlation-demo.ini" "${CMAKE_BINARY_DIR}/processing-output")
    set_tests_properties(processing_integration PROPERTIES TIMEOUT 30)
    add_executable(protocol_contract_tests tests/protocol_contract.cpp)
    target_link_libraries(protocol_contract_tests PRIVATE bus_core)
    add_test(NAME protocol_contract COMMAND protocol_contract_tests)
    add_executable(protocol_runtime_tests tests/protocol_runtime.cpp)
    target_link_libraries(protocol_runtime_tests PRIVATE bus_core)
    add_test(NAME protocol_runtime COMMAND protocol_runtime_tests)
    set_tests_properties(protocol_runtime PROPERTIES TIMEOUT 30)
    add_test(NAME protocol_cli COMMAND bus_protocol --demo)
    set_tests_properties(protocol_cli PROPERTIES TIMEOUT 30)
    add_test(NAME random_plugin_demo COMMAND bus_backend --demo $<TARGET_FILE:bus_random>)
    set_tests_properties(random_plugin_demo PROPERTIES TIMEOUT 30)
    add_executable(flat_export_tests tests/flat_export.cpp)
    target_link_libraries(flat_export_tests PRIVATE bus_core)
    add_test(NAME flat_export COMMAND flat_export_tests "${CMAKE_BINARY_DIR}/export-output")
    set_tests_properties(flat_export PROPERTIES TIMEOUT 30)
endif()
