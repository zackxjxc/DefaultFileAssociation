# Third-party dependencies for DefaultFileAssociation.
#
# The exact source revisions are pinned below so a clean configure is
# reproducible.  Both dependencies are header-only/interface targets.

include(FetchContent)
# 固定提交哈希，避免上游分支更新导致同一份项目构建出不同依赖；无需增加新的加密库。

option(DFA_FETCH_DEPENDENCIES
       "Download CLI11 and WIL when they are not already available"
       ON)

if(NOT DFA_FETCH_DEPENDENCIES)
    find_package(CLI11 CONFIG REQUIRED)
    find_package(WIL CONFIG REQUIRED)
    return()
endif()

# Do not build the upstream test, documentation, packaging, or example trees
# when these projects are embedded in this executable.
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG 919ce47dad39520a6e9a695d3e2d222c6920a445 # v2.7.2
    GIT_SHALLOW FALSE
)

FetchContent_Declare(
    WIL
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG cbf677fb0a942557d08fd129f4c106a76247b2ec # v1.0.260126.7
    GIT_SHALLOW FALSE
)

FetchContent_MakeAvailable(CLI11 WIL)
