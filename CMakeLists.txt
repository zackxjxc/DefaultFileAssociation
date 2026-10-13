cmake_minimum_required(VERSION 3.29)

project(DefaultFileAssociation
    VERSION 0.1.0
    DESCRIPTION "Register a Windows file association for the current user"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "DefaultFileAssociation only supports Windows.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    if(MSVC_VERSION GREATER_EQUAL 1950)
        message(FATAL_ERROR "Windows 7 requires the v143 toolset; use the supplied CMake presets.")
    endif()
    # 静态链接运行库，发布的工具无需额外安装 VC++ Redistributable。
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601)
# 这些宏限制源码可使用的 API，不能把预编译的新运行库变成 Win7 兼容运行库。

include(GNUInstallDirs)
include(cmake/Dependencies.cmake)
include(CTest)

set(DFA_SOURCES
    src/platform.cpp
    src/registry.cpp
    src/association.cpp
    src/browser.cpp
    src/diagnostics.cpp
    src/user_choice.cpp
    src/latest_native_probe.cpp
    src/msbuild_backend.cpp
    src/crypto/win_crypto.cpp
    src/crypto/user_choice_hash.cpp
    src/cli/command_line.cpp
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/msbuild_task.proj" DFA_MSBUILD_PROJECT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/msbuild_task.proj")
configure_file(src/msbuild_task_text.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/msbuild_task_text.h" @ONLY)
add_library(dfa_core STATIC ${DFA_SOURCES})
add_executable(DefaultFileAssociation src/main.cpp)
add_executable(DefaultFileAssociation::DefaultFileAssociation ALIAS
    DefaultFileAssociation)

target_compile_features(dfa_core PUBLIC cxx_std_23)
target_compile_definitions(dfa_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(dfa_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(dfa_core PUBLIC
    CLI11::CLI11
    WIL::WIL
    advapi32
    bcrypt
    crypt32
    ole32
    shell32
    shlwapi
    user32
)
target_link_libraries(DefaultFileAssociation PRIVATE dfa_core)

option(DFA_BUILD_HOST_PROBES "Build the controlled PDF association host experiment" OFF)
if(DFA_BUILD_HOST_PROBES)
    add_library(PdfAssociationHostProbe SHARED research/host-probes/native_host_probe.cpp research/host-probes/native_host_probe.def)
    target_link_libraries(PdfAssociationHostProbe PRIVATE dfa_core)
    add_executable(PdfAssociationApiProbe research/native-probes/assoc_handler_probe.cpp)
    target_link_libraries(PdfAssociationApiProbe PRIVATE shell32 ole32)
    if(MSVC)
        target_compile_options(PdfAssociationHostProbe PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(PdfAssociationApiProbe PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()

if(MSVC)
    target_compile_options(dfa_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(DefaultFileAssociation PRIVATE /W4 /permissive- /utf-8)
    target_link_options(DefaultFileAssociation PRIVATE
        "/MANIFESTUAC:level='asInvoker' uiAccess='false'")
    get_filename_component(msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(DFA_DUMPBIN NAMES dumpbin HINTS "${msvc_bin_dir}" REQUIRED)
    add_custom_command(TARGET DefaultFileAssociation POST_BUILD
        # 检查最终 EXE 的静态导入，发现已知 Win8+ API 则让构建失败。
        COMMAND "${CMAKE_COMMAND}"
            "-DDUMPBIN=${DFA_DUMPBIN}"
            "-DEXECUTABLE=$<TARGET_FILE:DefaultFileAssociation>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckWindowsImports.cmake"
        VERBATIM
    )
endif()

install(TARGETS DefaultFileAssociation
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_TESTING)
    # 测试使用临时键或进程内 HKCU 重定向；不会替换用户真实的 PDF 默认关联。
    add_executable(DefaultFileAssociationTests
        tests/user_choice_hash_tests.cpp
    )
    add_executable(CommandLineTests tests/command_line_tests.cpp)
    add_executable(RegistryTests tests/registry_tests.cpp)
    add_executable(UserChoiceTests tests/user_choice_tests.cpp)
    add_executable(AssociationTests tests/association_tests.cpp)
    add_executable(BrowserTests tests/browser_tests.cpp)
    # 所有测试复用同一份核心实现，避免测试与发布程序的编译配置发生偏差。
    foreach(test_target IN ITEMS DefaultFileAssociationTests CommandLineTests RegistryTests
            UserChoiceTests AssociationTests BrowserTests)
        target_link_libraries(${test_target} PRIVATE dfa_core)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /W4 /permissive- /utf-8)
        endif()
        add_test(NAME ${test_target} COMMAND ${test_target})
    endforeach()
endif()
