cmake_minimum_required(VERSION 3.28)

project(fssentry-minimal
    VERSION 0.1.0
    DESCRIPTION "fssentry — ETW file I/O monitor with versioning"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── x64 要求 ──────────────────────────────────────────────
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 (64-bit) is required")
endif()

# ── MSVC 编译选项 ─────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /utf-8 /wd4828)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
    add_definitions(-D_WIN32_WINNT=0x0A00)
endif()

# ── 可执行文件 ────────────────────────────────────────────
add_executable(fssentry
    main.cpp
    cli.cpp
    delta.cpp
    etw_capture.cpp
    process_tracker.cpp
    snapshot_store.cpp
    version_tracker.cpp
)

target_include_directories(fssentry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(fssentry PRIVATE
    bcrypt      # SHA-256
    advapi32    # ETW 会话管理
    tdh         # ETW 事件解码
)

if(MSVC)
    target_link_options(fssentry PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
