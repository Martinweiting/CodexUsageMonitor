cmake_minimum_required(VERSION 3.21)

project(CodexUsageMonitor VERSION 0.1.0 LANGUAGES CXX)

option(CODEX_USAGE_MONITOR_BUILD_TESTS "Build native CodexUsageMonitor tests" OFF)
option(CODEX_USAGE_MONITOR_UI_TEST_WINDOW "Expose the widget as an app window for UI automation" OFF)
set(CODEX_USAGE_MONITOR_UI_TEST_SCENARIO "0" CACHE STRING "UI test scenario: 0 quota, 1 activity, 2 simple, 3 taskbar, 4 activity zh, 5 quota on secondary monitor")

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
    src/LocalUsageStats.cpp
    src/WidgetPresentation.cpp
    src/ActivityAnalytics.cpp
    src/ActivityDetailsWindow.cpp
)

target_include_directories(CodexUsageMonitor PRIVATE src)

target_compile_definitions(CodexUsageMonitor PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    APP_VERSION_W=L\"${APP_VERSION_TEXT}\"
)

if(CODEX_USAGE_MONITOR_UI_TEST_WINDOW)
    target_compile_definitions(CodexUsageMonitor PRIVATE
        CODEX_USAGE_MONITOR_UI_TEST_WINDOW=1
        CODEX_USAGE_MONITOR_UI_TEST_SCENARIO=${CODEX_USAGE_MONITOR_UI_TEST_SCENARIO}
    )
endif()

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
    comctl32
    comdlg32
    oleacc
    uxtheme
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
        src/LocalUsageStats.cpp
        src/WidgetPresentation.cpp
    )
    target_include_directories(CodexUsageMonitorTests PRIVATE src)
    target_compile_definitions(CodexUsageMonitorTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    # Keep fixture assertions active in Release so CTest verifies behavior in both configurations.
    target_compile_options(CodexUsageMonitorTests PRIVATE /utf-8 /UNDEBUG)
    target_link_libraries(CodexUsageMonitorTests PRIVATE
        crypt32
        shlwapi
        winhttp
    )
    add_test(NAME WidgetPresentationTests COMMAND CodexUsageMonitorTests)
    add_executable(ActivityAnalyticsTests tests/ActivityAnalyticsTests.cpp src/ActivityAnalytics.cpp src/LocalUsageStats.cpp src/JsonLite.cpp)
    target_include_directories(ActivityAnalyticsTests PRIVATE src)
    target_compile_definitions(ActivityAnalyticsTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(ActivityAnalyticsTests PRIVATE /utf-8 /UNDEBUG)
    target_link_libraries(ActivityAnalyticsTests PRIVATE shell32 ole32 advapi32)
    add_test(NAME ActivityAnalyticsTests COMMAND ActivityAnalyticsTests)
    add_executable(WindowInteractionTests
        tests/WindowInteractionTests.cpp
        src/AppBarWindow.cpp
        src/CodexUsageFetcher.cpp
        src/JsonLite.cpp
        src/LocalUsageStats.cpp
        src/WidgetPresentation.cpp
        src/ActivityAnalytics.cpp
        src/ActivityDetailsWindow.cpp
    )
    target_include_directories(WindowInteractionTests PRIVATE src)
    target_compile_definitions(WindowInteractionTests PRIVATE
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN APP_VERSION_W=L\"test\")
    target_compile_options(WindowInteractionTests PRIVATE /utf-8 /UNDEBUG)
    target_link_libraries(WindowInteractionTests PRIVATE
        advapi32 crypt32 d2d1 dwrite dwmapi gdi32 ole32 shell32 shlwapi user32 winhttp windowscodecs comctl32 comdlg32 oleacc uxtheme)
    add_test(NAME WindowInteractionTests COMMAND WindowInteractionTests)
endif()
