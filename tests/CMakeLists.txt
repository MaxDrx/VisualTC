find_package(Catch2 3 QUIET CONFIG)
if(NOT Catch2_FOUND)
    if(NOT VISUALTC_FETCH_CATCH2)
        message(FATAL_ERROR "Catch2 3 not found and VISUALTC_FETCH_CATCH2=OFF")
    endif()
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()
include(Catch)

add_library(visualtc_fixtures STATIC fixtures/DicomFixtures.cpp)
target_include_directories(visualtc_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(visualtc_fixtures SYSTEM PRIVATE ${GDCM_INCLUDE_DIRS})
target_link_libraries(visualtc_fixtures PUBLIC VisualTC::core PRIVATE gdcmMSFF LibArchive::LibArchive)

add_executable(visualtc_tests
    unit/test_textutil.cpp
    unit/test_window_level.cpp
    unit/test_measurements.cpp
    unit/test_orientation.cpp
    unit/test_sorting_geometry.cpp
    unit/test_dicom_io.cpp
    unit/test_mpr.cpp
    unit/test_cache.cpp
    unit/test_worker_protocol.cpp
    unit/test_sync.cpp
    unit/test_archive.cpp
)
target_link_libraries(visualtc_tests PRIVATE visualtc_fixtures VisualTC::core Catch2::Catch2WithMain)
if(NOT MSVC)
    target_compile_options(visualtc_tests PRIVATE -Wno-missing-field-initializers)
endif()
catch_discover_tests(visualtc_tests DISCOVERY_MODE PRE_TEST)

# Synthetic demo/QA exam generator (no real patient data).
add_executable(make_phantom ${CMAKE_SOURCE_DIR}/tools/make_phantom.cpp)
target_link_libraries(make_phantom PRIVATE visualtc_fixtures VisualTC::core)

# Qt integration tests (headless): viewport interaction, sync, MPR, isolation.
if(VISUALTC_BUILD_APP)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(visualtc_ui_tests ui/test_ui.cpp)
    set_target_properties(visualtc_ui_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(visualtc_ui_tests PRIVATE visualtc_ui visualtc_fixtures Qt6::Test)
    target_compile_definitions(visualtc_ui_tests PRIVATE VISUALTC_WORKER_PATH="$<TARGET_FILE:visualtc-worker>")
    add_dependencies(visualtc_ui_tests visualtc-worker)
    # Results also go to a file (flushed line by line, survives a crash), and
    # Qt's own messages to stderr: on Windows, without a console, Qt sends
    # them to the debugger output, where CI cannot see them.
    add_test(NAME ui_integration
             COMMAND visualtc_ui_tests -o ${CMAKE_BINARY_DIR}/ui_integration.log,txt -o -,txt)
    set_tests_properties(ui_integration PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
endif()

# Core performance probe (decode, window/level, MPR) on any folder.
add_executable(vtc_bench ${CMAKE_SOURCE_DIR}/tools/vtc_bench.cpp)
target_link_libraries(vtc_bench PRIVATE VisualTC::core)

# Every text of the interface has its Spanish and English translation
# (resources/i18n/*.json), with the same %1/%n placeholders.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME translations_complete
             COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/i18n.py check)
endif()
