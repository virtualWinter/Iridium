cmake_minimum_required(VERSION 3.21)
project(Iridium VERSION 0.0.1 LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Network Sql Svg Test Widgets)
find_package(KF6Svg REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0 wpe-platform-2.0)
pkg_check_modules(WPE_FDO REQUIRED IMPORTED_TARGET wpebackend-fdo-1.0)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)

qt_standard_project_setup()

set(ENGINE_VIEW_SOURCES
    src/engine/webkit/WebKitView.cpp
    src/engine/webkit/EglPresenter.cpp
    src/engine/webkit/PreferredColorScheme.cpp
)

# The main window and the right-hand panel. They are a group because they are
# always built together: the panel is owned by the window, so any target that
# compiles MainWindow needs it.
set(UI_MAIN_SOURCES
    src/ui/MainWindow.cpp
    src/ui/RightSidebar.cpp
)

# Per-profile data. Both are needed by the settings window as well as the
# browser, so they are a named group.
set(PROFILE_HISTORY_SOURCES
    src/browser/HistoryStore.cpp
    src/browser/ProfileManager.cpp
)

# The extensions layer. WebKit's WebKitWebExtension types are non-functional in
# the installed WPE build, so manifests, match patterns and content-script
# injection are implemented here instead of delegating to the engine.
set(SETTINGS_UI_SOURCES
    src/ui/settings/AppearancePage.cpp
    src/ui/settings/GeneralPage.cpp
    src/ui/settings/HistoryPage.cpp
    src/ui/settings/ProfilesPage.cpp
    src/ui/settings/ExtensionsPage.cpp
    src/ui/settings/SettingsStore.cpp
    src/ui/settings/SettingsWindow.cpp
)

set(EXTENSIONS_CORE_SOURCES
    src/extensions/Manifest.cpp
    src/extensions/MatchPattern.cpp
    src/extensions/ExtensionRegistry.cpp
    src/extensions/ExtensionPaths.cpp
    src/extensions/ExtensionInstaller.cpp
    src/extensions/ContentScriptInjector.cpp
)

# The installer reads an update manifest over http, so it needs QtNetwork.
set(EXTENSIONS_CORE_LIBRARIES Qt6::Core Qt6::Network)

# The API layer needs the engine's script bridge, so it is a separate group.
set(EXTENSIONS_SOURCES
    ${EXTENSIONS_CORE_SOURCES}
    src/extensions/ExtensionApi.cpp
    src/extensions/ExtensionHost.cpp
)

# The API layer is engine-independent; only the host and the WebKit bridge need
# WebKit, which is why they are listed separately above.
set(EXTENSIONS_WEBKIT_SOURCES
    ${EXTENSIONS_SOURCES}
    src/engine/webkit/WebKitScriptBridge.cpp
)

add_executable(iridium
    src/main.cpp
    src/app/Application.cpp
    src/browser/Browser.cpp
    ${PROFILE_HISTORY_SOURCES}
    ${UI_MAIN_SOURCES}
    src/ui/RightSidebar.cpp
    src/ui/decoration/decorationtheme.cpp
    src/ui/decoration/windowdecoration.cpp
    ${SETTINGS_UI_SOURCES}
    ${ENGINE_VIEW_SOURCES}
    ${EXTENSIONS_WEBKIT_SOURCES}
)
target_include_directories(iridium PRIVATE src)
target_compile_definitions(iridium PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
target_link_libraries(iridium PRIVATE Qt6::Network Qt6::Sql Qt6::Svg Qt6::Widgets KF6::Svg PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER PkgConfig::EGL PkgConfig::GLES)
target_compile_options(iridium PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(wpe-platform-probe tests/wpe_platform_probe.cpp)
    target_link_libraries(wpe-platform-probe PRIVATE PkgConfig::WPE)
    target_compile_options(wpe-platform-probe PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME wpe-platform-probe COMMAND wpe-platform-probe)

    # The extensions architecture rests on a few facts about this engine build
    # (named script worlds exist, the engine's own extension types do not work,
    # there is no SoupSession accessor). This asserts them, so an engine upgrade
    # that changes one fails here instead of quietly invalidating the design.
    add_executable(webkit-extension-probe tests/webkit_extension_probe.cpp)
    target_link_libraries(webkit-extension-probe PRIVATE PkgConfig::WPE)
    target_compile_options(webkit-extension-probe PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME webkit-extension-probe COMMAND webkit-extension-probe)

    # The API registry and dispatcher: the framework every namespace registers
    # into, and the single place a call is checked and dispatched. Engine-free,
    # because everything it depends on is either the generated catalog or the
    # extension registry.
    add_executable(extensions-dispatcher-test
        tests/extensions_dispatcher_test.cpp
        src/extensions/generated/ApiCatalog.cpp
        src/extensions/api/ApiRegistry.cpp
        src/extensions/api/ApiDispatcher.cpp
        ${EXTENSIONS_CORE_SOURCES})
    target_include_directories(extensions-dispatcher-test PRIVATE src)
    target_link_libraries(extensions-dispatcher-test PRIVATE ${EXTENSIONS_CORE_LIBRARIES})
    target_compile_options(extensions-dispatcher-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME extensions-dispatcher COMMAND extensions-dispatcher-test)

    # The API inventory and the coverage documents are generated from pinned
    # upstream definitions plus a hand-written status database. This checks the
    # database against the inventory (an API with no status, or a 'full' with no
    # test, fails) and that the committed documents are current.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME extensions-coverage
            COMMAND ${Python3_EXECUTABLE} tools/webext/coverage.py --check
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        # The C++ catalog is generated from the same inventory, so a refreshed
        # inventory cannot leave stale generated code behind.
        add_test(NAME extensions-codegen
            COMMAND ${Python3_EXECUTABLE} tools/webext/codegen.py --check
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()

    # Engine tests build the real WebKitView and run headless. They report
    # results through document.title/callbacks and skip themselves when the
    # environment cannot provide what they need.
    function(iridium_add_webview_test target test_name source)
        add_executable(${target} ${source} ${ENGINE_VIEW_SOURCES})
        target_include_directories(${target} PRIVATE src)
        target_compile_definitions(${target} PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
        target_link_libraries(${target} PRIVATE
            Qt6::Widgets PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
            PkgConfig::EGL PkgConfig::GLES
            ${ARGN})
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endfunction()

    iridium_add_webview_test(webview-pointer-test webview-pointer
        tests/webview_pointer_test.cpp)
    iridium_add_webview_test(webview-gl-test webview-gl
        tests/webview_gl_test.cpp)
    iridium_add_webview_test(webview-theme-test webview-theme
        tests/webview_theme_test.cpp)
    iridium_add_webview_test(webview-download-test webview-download
        tests/webview_download_test.cpp)
    iridium_add_webview_test(webview-render-test webview-render
        tests/webview_render_test.cpp)
    iridium_add_webview_test(webview-useragent-test webview-useragent
        tests/webview_useragent_test.cpp)

    # The registry test exercises enable/disable, persistence, matching and
    # path-traversal refusal against real on-disk fixtures.
    add_executable(extensions-registry-test
        tests/extensions_registry_test.cpp
        ${EXTENSIONS_CORE_SOURCES})
    target_include_directories(extensions-registry-test PRIVATE src)
    target_link_libraries(extensions-registry-test PRIVATE ${EXTENSIONS_CORE_LIBRARIES})
    target_compile_options(extensions-registry-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME extensions-registry COMMAND extensions-registry-test)

    # XDG path resolution and install/uninstall, which touch the filesystem.
    add_executable(extensions-paths-test
        tests/extensions_paths_test.cpp
        ${EXTENSIONS_CORE_SOURCES})
    target_include_directories(extensions-paths-test PRIVATE src)
    target_link_libraries(extensions-paths-test PRIVATE ${EXTENSIONS_CORE_LIBRARIES})
    target_compile_options(extensions-paths-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME extensions-paths COMMAND extensions-paths-test)

    # The browser.* API surface, driven directly against a fake host.
    add_executable(extensions-api-test
        tests/extensions_api_test.cpp
        ${EXTENSIONS_CORE_SOURCES}
        src/extensions/ExtensionApi.cpp)
    target_compile_definitions(extensions-api-test PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
    target_include_directories(extensions-api-test PRIVATE src)
    target_link_libraries(extensions-api-test PRIVATE ${EXTENSIONS_CORE_LIBRARIES})
    target_compile_options(extensions-api-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME extensions-api COMMAND extensions-api-test)

    # The settings panes as widgets: list contents, filtering, toggling, and
    # install/remove round trips against real fixtures.
    # The right-hand panel as a widget. It needs no browser, no engine and no
    # profile, which is the property being tested: the panel is a view.
    add_executable(right-sidebar-test
        tests/right_sidebar_test.cpp
        src/ui/RightSidebar.cpp)
    target_include_directories(right-sidebar-test PRIVATE src)
    set_target_properties(right-sidebar-test PROPERTIES AUTOMOC ON)
    # QSignalSpy lives in Qt6::Test, not Qt6::Widgets.
    target_link_libraries(right-sidebar-test PRIVATE Qt6::Widgets Qt6::Test)
    target_compile_options(right-sidebar-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME right-sidebar COMMAND right-sidebar-test)
    set_tests_properties(right-sidebar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The settings window drives MainWindow, so this target links the browser
    # UI as well as the panes.
    add_executable(settings-test
        tests/settings_test.cpp
        ${UI_MAIN_SOURCES}
        src/browser/Browser.cpp
        ${EXTENSIONS_CORE_SOURCES}
        src/extensions/ExtensionApi.cpp
        src/extensions/ExtensionHost.cpp
        src/engine/webkit/WebKitScriptBridge.cpp
        ${ENGINE_VIEW_SOURCES}
        ${PROFILE_HISTORY_SOURCES}
        ${SETTINGS_UI_SOURCES}
        src/ui/decoration/decorationtheme.cpp
        src/ui/decoration/windowdecoration.cpp)
    target_include_directories(settings-test PRIVATE src)
    target_compile_definitions(settings-test PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
    set_target_properties(settings-test PROPERTIES AUTOMOC ON)
    target_link_libraries(settings-test PRIVATE
        Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Svg ${EXTENSIONS_CORE_LIBRARIES} KF6::Svg
        PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
        PkgConfig::EGL PkgConfig::GLES)
    target_compile_options(settings-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings COMMAND settings-test)
    set_tests_properties(settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # End-to-end: a content script from a real on-disk extension must run in a
    # matching page. Needs the engine, so it uses the webview harness plus the
    # extensions sources.
    add_executable(webview-extension-test
        tests/webview_extension_test.cpp
        ${ENGINE_VIEW_SOURCES}
        ${EXTENSIONS_WEBKIT_SOURCES})
    target_include_directories(webview-extension-test PRIVATE src)
    target_compile_definitions(webview-extension-test PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
    target_link_libraries(webview-extension-test PRIVATE
        Qt6::Widgets Qt6::Sql ${EXTENSIONS_CORE_LIBRARIES}
        PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
        PkgConfig::EGL PkgConfig::GLES)
    target_compile_options(webview-extension-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME webview-extension COMMAND webview-extension-test)
    set_tests_properties(webview-extension PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # History recording through the real window, so the shipping code path is
    # what gets exercised.
    iridium_add_webview_test(webview-history-test webview-history
        tests/webview_history_test.cpp)
    # The real MainWindow, so history is recorded by the shipping code path.
    target_sources(webview-history-test PRIVATE
        ${UI_MAIN_SOURCES}
        src/browser/Browser.cpp
        src/ui/decoration/decorationtheme.cpp
        src/ui/decoration/windowdecoration.cpp
        ${PROFILE_HISTORY_SOURCES}
        ${SETTINGS_UI_SOURCES}
        ${EXTENSIONS_WEBKIT_SOURCES})
    target_link_libraries(webview-history-test PRIVATE
        KF6::Svg Qt6::Network Qt6::Sql Qt6::Svg
        PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
        PkgConfig::EGL PkgConfig::GLES)

    # Manifest parsing and match-pattern matching are pure logic: no engine, no
    # display, so they get their own lightweight target.
    add_executable(extensions-manifest-test
        tests/extensions_manifest_test.cpp
        src/extensions/Manifest.cpp
        src/extensions/MatchPattern.cpp)
    target_include_directories(extensions-manifest-test PRIVATE src)
    target_link_libraries(extensions-manifest-test PRIVATE ${EXTENSIONS_CORE_LIBRARIES})
    target_compile_options(extensions-manifest-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME extensions-manifest COMMAND extensions-manifest-test)

    # The settings window as a whole: every category builds and is selectable,
    # and the live-state pages refresh when the window is shown.
    iridium_add_webview_test(settings-window-categories-test settings-window-categories
        tests/settings_window_categories_test.cpp)
    target_sources(settings-window-categories-test PRIVATE
        ${UI_MAIN_SOURCES}
        src/browser/Browser.cpp
        src/ui/decoration/decorationtheme.cpp
        src/ui/decoration/windowdecoration.cpp
        ${PROFILE_HISTORY_SOURCES}
        ${SETTINGS_UI_SOURCES}
        ${EXTENSIONS_WEBKIT_SOURCES})
    target_link_libraries(settings-window-categories-test PRIVATE
        KF6::Svg Qt6::Network Qt6::Sql Qt6::Svg
        PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
        PkgConfig::EGL PkgConfig::GLES)

    # The settings store is profile-scoped, so it needs a profile to be pointed
    # at before it will read or write anything.
    add_executable(settings-store-test
        tests/settings_store_test.cpp
        src/ui/settings/SettingsStore.cpp)
    target_include_directories(settings-store-test PRIVATE src)
    target_link_libraries(settings-store-test PRIVATE Qt6::Core Qt6::Test)
    target_compile_options(settings-store-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings-store COMMAND settings-store-test)

    # Profiles and history need neither the engine nor a display.
    add_executable(browser-data-test
        tests/browser_data_test.cpp
        src/browser/HistoryStore.cpp
        src/browser/ProfileManager.cpp)
    target_include_directories(browser-data-test PRIVATE src)
    target_link_libraries(browser-data-test PRIVATE Qt6::Core Qt6::Sql)
    target_compile_options(browser-data-test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME browser-data COMMAND browser-data-test)

    pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET gstreamer-1.0)
    if(GSTREAMER_FOUND)
        iridium_add_webview_test(webview-media-test webview-media
            tests/webview_media_test.cpp PkgConfig::GSTREAMER)
        set_tests_properties(webview-media PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Benchmark tool (not a CTest test): network + minutes.
    add_executable(speedometer-benchmark
        tests/speedometer_benchmark.cpp
        ${ENGINE_VIEW_SOURCES})
    target_include_directories(speedometer-benchmark PRIVATE src)
    target_compile_definitions(speedometer-benchmark PRIVATE IRIDIUM_VERSION="${PROJECT_VERSION}")
    target_link_libraries(speedometer-benchmark PRIVATE
        Qt6::Widgets PkgConfig::WPE PkgConfig::WPE_FDO PkgConfig::WAYLAND_SERVER
        PkgConfig::EGL PkgConfig::GLES)
    target_compile_options(speedometer-benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()
