cmake_minimum_required(VERSION 3.21)

project(CodexUsageMonitor VERSION 0.1.0 LANGUAGES CXX)

option(CODEX_USAGE_MONITOR_BUILD_TESTS "Build native CodexUsageMonitor tests" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(APP_VERSION_TEXT "")
if(DEFINED APP_VERSION AND NOT APP_VERSION STREQUAL "")
    set(APP_VERSION_TEXT "${APP_VERSION}")
elseif(DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
    set(APP_VERSION_TEXT "$ENV{GITHUB_REF_NAME}")
else()
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE APP_VERSION_TEXT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

if(APP_VERSION_TEXT STREQUAL "")
    set(APP_VERSION_TEXT "dev")
endif()

add_executable(CodexUsageMonitor WIN32
    src/main.cpp
    src/AppBarWindow.cpp
    src/CodexUsageFetcher.cpp
    src/JsonLite.cpp
    src/WidgetPresentation.cpp
)

target_include_directories(CodexUsageMonitor PRIVATE src)

target_compile_definitions(CodexUsageMonitor PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    APP_VERSION_W=L\"${APP_VERSION_TEXT}\"
)

target_compile_options(CodexUsageMonitor PRIVATE /utf-8)

target_link_libraries(CodexUsageMonitor PRIVATE
    advapi32
    crypt32
    d2d1
    dwrite
    dwmapi
    gdi32
    ole32
    shell32
    shlwapi
    user32
    winhttp
    windowscodecs
)

add_custom_command(TARGET CodexUsageMonitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:CodexUsageMonitor>/assets"
)

if(CODEX_USAGE_MONITOR_BUILD_TESTS)
    enable_testing()
    add_executable(CodexUsageMonitorTests
        tests/WidgetPresentationTests.cpp
        src/CodexUsageFetcher.cpp
        src/JsonLite.cpp
        src/WidgetPresentation.cpp
    )
    target_include_directories(CodexUsageMonitorTests PRIVATE src)
    target_compile_definitions(CodexUsageMonitorTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(CodexUsageMonitorTests PRIVATE /utf-8)
    target_link_libraries(CodexUsageMonitorTests PRIVATE
        crypt32
        shlwapi
        winhttp
    )
    add_test(NAME WidgetPresentationTests COMMAND CodexUsageMonitorTests)
endif()
