cmake_minimum_required(VERSION 3.24)
project(DiskUtilities VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
find_package(Threads REQUIRED)

option(DISKUTILITIES_BUILD_GUI "Build the WebUI desktop application" ON)
option(DISKUTILITIES_BUILD_CLI "Build the standalone filesystem scanner CLI" ON)
option(DISKUTILITIES_BUILD_TESTS "Build unit tests" ON)
option(DISKUTILITIES_FETCH_DEPENDENCIES "Fetch JsonCpp and WebUI when they are not installed" ON)
option(DISKUTILITIES_INSTALL "Generate install rules and the find_package(DiskUtilities) config" ON)
set(DISKUTILITIES_WITH_TK4CPP "AUTO" CACHE STRING "Use the TK4CPP Thread and Utility modules: ON, OFF or AUTO (when found)")
set_property(CACHE DISKUTILITIES_WITH_TK4CPP PROPERTY STRINGS AUTO ON OFF)
set(DISKUTILITIES_SANITIZER "" CACHE STRING "Sanitizer to enable: address, undefined, thread or address,undefined")
# WebUI tags stop at 2.5.0-beta.3, which lacks the C++ bind(name, instance, method) overload
# used by WebUiAdapter. Pin an exact commit of the main branch instead.
set(DISKUTILITIES_WEBUI_GIT_TAG "efac0e24dd4e6199dd7308c485906c90fb8816a2" CACHE STRING "WebUI revision to fetch")
set(DISKUTILITIES_JSONCPP_GIT_TAG "1.9.6" CACHE STRING "JsonCpp revision to fetch")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Compile flags shared by every target of this project.
function(diskutilities_configure target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(DISKUTILITIES_SANITIZER)
            target_compile_options(${target} PRIVATE -fsanitize=${DISKUTILITIES_SANITIZER} -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=${DISKUTILITIES_SANITIZER})
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------------
# Core library: no third-party dependency, so it can be installed and exported.
# ---------------------------------------------------------------------------------
add_library(diskutilities_core
    src/core/Executor.cpp
    src/filesystem/EntryStat.cpp
    src/filesystem/DirectoryScanner.cpp
    src/filesystem/FolderBrowser.cpp
    src/filesystem/FileSearcher.cpp
    src/analysis/Snapshot.cpp
    src/bridge/SnapshotBridge.cpp
    src/web/ScanController.cpp
    src/web/SearchController.cpp
)
add_library(DiskUtilities::Core ALIAS diskutilities_core)
set_target_properties(diskutilities_core PROPERTIES EXPORT_NAME Core OUTPUT_NAME diskutilities_core)
target_include_directories(diskutilities_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(diskutilities_core PUBLIC Threads::Threads)
target_compile_features(diskutilities_core PUBLIC cxx_std_23)
diskutilities_configure(diskutilities_core)

# ---------------------------------------------------------------------------------
# Optional TK4CPP integration (Thread: ThreadPool, Utility: System/FolderHandler).
# Point CMAKE_PREFIX_PATH or TK4CPP_DIR at an installed TK4CPP package.
# ---------------------------------------------------------------------------------
set(DISKUTILITIES_HAS_TK4CPP OFF)
if(NOT DISKUTILITIES_WITH_TK4CPP STREQUAL "OFF")
    if(DISKUTILITIES_WITH_TK4CPP STREQUAL "ON")
        find_package(TK4CPP CONFIG REQUIRED COMPONENTS Thread Utility)
    else()
        find_package(TK4CPP CONFIG QUIET COMPONENTS Thread Utility)
    endif()
    # Shared builds expose TK4CPP::<Name>, static ones TK4CPP::<Name>_static.
    foreach(_module Thread Utility)
        set(DISKUTILITIES_TK4CPP_${_module}_TARGET "")
        foreach(_candidate TK4CPP::${_module} TK4CPP::${_module}_static)
            if(NOT DISKUTILITIES_TK4CPP_${_module}_TARGET AND TARGET ${_candidate})
                set(DISKUTILITIES_TK4CPP_${_module}_TARGET ${_candidate})
            endif()
        endforeach()
    endforeach()
    if(DISKUTILITIES_TK4CPP_Thread_TARGET AND DISKUTILITIES_TK4CPP_Utility_TARGET)
        set(DISKUTILITIES_HAS_TK4CPP ON)
    elseif(DISKUTILITIES_WITH_TK4CPP STREQUAL "ON")
        message(FATAL_ERROR "TK4CPP found but the Thread/Utility targets are missing.")
    endif()
endif()
message(STATUS "DiskUtilities: TK4CPP integration ${DISKUTILITIES_HAS_TK4CPP}")

if(DISKUTILITIES_HAS_TK4CPP)
    # Not part of the exported core: it links TK4CPP, the core must stay dependency free.
    add_library(diskutilities_tk4cpp STATIC src/tk4cpp/Tk4cppSupport.cpp)
    target_include_directories(diskutilities_tk4cpp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(diskutilities_tk4cpp PUBLIC diskutilities_core
        ${DISKUTILITIES_TK4CPP_Thread_TARGET} ${DISKUTILITIES_TK4CPP_Utility_TARGET})
    target_compile_definitions(diskutilities_tk4cpp PUBLIC DISKUTILITIES_HAS_TK4CPP=1)
    diskutilities_configure(diskutilities_tk4cpp)
endif()

# ---------------------------------------------------------------------------------
# JsonCpp (only needed by the applications and the JSON tests).
# ---------------------------------------------------------------------------------
set(DISKUTILITIES_JSON_TARGET "")
if(DISKUTILITIES_BUILD_GUI OR DISKUTILITIES_BUILD_CLI OR DISKUTILITIES_BUILD_TESTS)
    find_package(jsoncpp CONFIG QUIET)
    if(TARGET JsonCpp::JsonCpp)
        set(DISKUTILITIES_JSON_TARGET JsonCpp::JsonCpp)
    elseif(TARGET jsoncpp_static)
        set(DISKUTILITIES_JSON_TARGET jsoncpp_static)
    elseif(TARGET jsoncpp_lib)
        set(DISKUTILITIES_JSON_TARGET jsoncpp_lib)
    elseif(DISKUTILITIES_FETCH_DEPENDENCIES)
        function(diskutilities_fetch_jsoncpp)
            set(JSONCPP_WITH_TESTS OFF)
            set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF)
            set(JSONCPP_WITH_EXAMPLE OFF)
            set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF)
            set(JSONCPP_WITH_CMAKE_PACKAGE OFF)
            set(BUILD_SHARED_LIBS OFF)
            set(BUILD_OBJECT_LIBS OFF)
            FetchContent_Declare(jsoncpp
                GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
                GIT_TAG ${DISKUTILITIES_JSONCPP_GIT_TAG}
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(jsoncpp)
        endfunction()
        diskutilities_fetch_jsoncpp()
        if(TARGET jsoncpp_static)
            set(DISKUTILITIES_JSON_TARGET jsoncpp_static)
        elseif(TARGET jsoncpp_lib)
            set(DISKUTILITIES_JSON_TARGET jsoncpp_lib)
        endif()
    endif()
    if(NOT DISKUTILITIES_JSON_TARGET)
        message(FATAL_ERROR "JsonCpp not found: install it or enable DISKUTILITIES_FETCH_DEPENDENCIES.")
    endif()

    # Bridge DTO <-> JSON; shared by the applications and the tests, never installed.
    add_library(diskutilities_json STATIC src/web/JsonSerialization.cpp)
    target_include_directories(diskutilities_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/web)
    target_link_libraries(diskutilities_json PUBLIC diskutilities_core ${DISKUTILITIES_JSON_TARGET})
    diskutilities_configure(diskutilities_json)
endif()

# ---------------------------------------------------------------------------------
# Command-line scanner and file search
# ---------------------------------------------------------------------------------
if(DISKUTILITIES_BUILD_CLI OR DISKUTILITIES_BUILD_TESTS)
    # Argument parsing of `diskutilities-cli find`; separate so it can be unit tested.
    add_library(diskutilities_cli_args STATIC src/cli/FindArguments.cpp)
    target_include_directories(diskutilities_cli_args PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/cli)
    target_link_libraries(diskutilities_cli_args PUBLIC diskutilities_core)
    if(DISKUTILITIES_HAS_TK4CPP)
        target_link_libraries(diskutilities_cli_args PUBLIC diskutilities_tk4cpp)
    endif()
    diskutilities_configure(diskutilities_cli_args)
endif()
if(DISKUTILITIES_BUILD_CLI)
    add_executable(diskutilities-cli src/cli/main.cpp)
    target_link_libraries(diskutilities-cli PRIVATE diskutilities_json diskutilities_cli_args)
    if(DISKUTILITIES_HAS_TK4CPP)
        target_link_libraries(diskutilities-cli PRIVATE diskutilities_tk4cpp)
    endif()
    if(MINGW)
        target_link_options(diskutilities-cli PRIVATE -municode) # wmain entry point
    endif()
    diskutilities_configure(diskutilities-cli)
endif()

# ---------------------------------------------------------------------------------
# WebUI desktop application
# ---------------------------------------------------------------------------------
if(DISKUTILITIES_BUILD_GUI)
    # An installed WebUI built with TLS lists OpenSSL::SSL / OpenSSL::Crypto in its link interface but its
    # webui-config.cmake does not call find_dependency(OpenSSL). Define those targets first, otherwise
    # find_package(webui) aborts with "target OpenSSL::SSL was not found". Harmless when OpenSSL is absent.
    find_package(OpenSSL QUIET)
    find_package(webui CONFIG QUIET)
    if(TARGET webui::webui)
        set(DISKUTILITIES_WEBUI_TARGET webui::webui)
    elseif(TARGET webui)
        set(DISKUTILITIES_WEBUI_TARGET webui)
    elseif(DISKUTILITIES_FETCH_DEPENDENCIES)
        function(diskutilities_fetch_webui)
            set(WEBUI_BUILD_EXAMPLES OFF)
            set(BUILD_SHARED_LIBS OFF)
            FetchContent_Declare(webui
                GIT_REPOSITORY https://github.com/webui-dev/webui.git
                GIT_TAG ${DISKUTILITIES_WEBUI_GIT_TAG})
            FetchContent_MakeAvailable(webui)
        endfunction()
        diskutilities_fetch_webui()
        if(TARGET webui)
            set(DISKUTILITIES_WEBUI_TARGET webui)
        endif()
    endif()
    if(NOT DISKUTILITIES_WEBUI_TARGET)
        message(FATAL_ERROR "WebUI not found: install it or enable DISKUTILITIES_FETCH_DEPENDENCIES.")
    endif()

    file(TO_CMAKE_PATH "${CMAKE_INSTALL_FULL_DATADIR}/DiskUtilities/web" DISKUTILITIES_INSTALL_WEB_DIR_CMAKE)
    add_executable(diskutilities src/main.cpp src/web/WebUiAdapter.cpp)
    target_link_libraries(diskutilities PRIVATE diskutilities_json ${DISKUTILITIES_WEBUI_TARGET} ${CMAKE_DL_LIBS})
    if(DISKUTILITIES_HAS_TK4CPP)
        target_link_libraries(diskutilities PRIVATE diskutilities_tk4cpp)
    endif()
    target_compile_definitions(diskutilities PRIVATE
        DISKUTILITIES_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
        DISKUTILITIES_INSTALL_WEB_DIR="${DISKUTILITIES_INSTALL_WEB_DIR_CMAKE}")
    diskutilities_configure(diskutilities)
endif()

# ---------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------
if(DISKUTILITIES_BUILD_TESTS AND BUILD_TESTING)
    function(diskutilities_add_test name)
        add_executable(${name} tests/${name}.cpp)
        target_include_directories(${name} PRIVATE tests)
        target_link_libraries(${name} PRIVATE diskutilities_json)
        diskutilities_configure(${name})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    diskutilities_add_test(executor_tests)
    diskutilities_add_test(scanner_tests)
    diskutilities_add_test(snapshot_tests)
    diskutilities_add_test(bridge_tests)
    diskutilities_add_test(controller_tests)
    diskutilities_add_test(json_tests)
    diskutilities_add_test(path_tests)
    diskutilities_add_test(folder_tests)
    diskutilities_add_test(glob_tests)
    diskutilities_add_test(search_tests)
    diskutilities_add_test(search_controller_tests)
    add_executable(cli_tests tests/cli_tests.cpp)
    target_include_directories(cli_tests PRIVATE tests)
    target_link_libraries(cli_tests PRIVATE diskutilities_cli_args)
    diskutilities_configure(cli_tests)
    add_test(NAME cli_tests COMMAND cli_tests)

    if(DISKUTILITIES_HAS_TK4CPP)
        add_executable(tk4cpp_tests tests/tk4cpp_tests.cpp)
        target_include_directories(tk4cpp_tests PRIVATE tests)
        target_link_libraries(tk4cpp_tests PRIVATE diskutilities_tk4cpp diskutilities_cli_args)
        diskutilities_configure(tk4cpp_tests)
        add_test(NAME tk4cpp_tests COMMAND tk4cpp_tests)
    endif()

    # The same scanner tests with the portable (std::filesystem only) stat path.
    add_executable(scanner_tests_portable tests/scanner_tests.cpp src/filesystem/EntryStat.cpp)
    target_include_directories(scanner_tests_portable PRIVATE tests)
    target_compile_definitions(scanner_tests_portable PRIVATE DISKUTILITIES_DISABLE_POSIX_STAT)
    target_link_libraries(scanner_tests_portable PRIVATE diskutilities_core)
    diskutilities_configure(scanner_tests_portable)
    add_test(NAME scanner_tests_portable COMMAND scanner_tests_portable)
endif()

# ---------------------------------------------------------------------------------
# Install + find_package(DiskUtilities)
# ---------------------------------------------------------------------------------
if(DISKUTILITIES_INSTALL)
    install(TARGETS diskutilities_core EXPORT DiskUtilitiesTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT DiskUtilitiesTargets NAMESPACE DiskUtilities:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DiskUtilities)
    configure_package_config_file(cmake/DiskUtilitiesConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/DiskUtilitiesConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DiskUtilities)
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/DiskUtilitiesConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/DiskUtilitiesConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/DiskUtilitiesConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DiskUtilities)
    install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/DiskUtilities/web)
    if(TARGET diskutilities)
        install(TARGETS diskutilities RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    if(TARGET diskutilities-cli)
        install(TARGETS diskutilities-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()
