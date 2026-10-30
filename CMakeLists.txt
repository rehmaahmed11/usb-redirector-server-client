cmake_minimum_required(VERSION 3.20)
project(MiniUsbBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
  message(FATAL_ERROR "Mini USB Bridge targets Windows (Win32, Winsock, SChannel, SetupAPI).")
endif()

# ---------------------------------------------------------------------------
# Minimum Windows version of the produced binary
# ---------------------------------------------------------------------------
#   win10 (default) : normal build (Windows 10/11; also runs on 7/8.1 because the
#                     code goes through src/win/wincompat.cpp for newer APIs --
#                     ci/check_legacy_compat.py keeps it that way)
#   vista           : _WIN32_WINNT=0x0600 (Windows Vista / Server 2008 and later)
#   xp              : _WIN32_WINNT=0x0501 (Windows XP SP3 / Server 2003 and later)
#                     MSVC needs the "v141_xp" toolset and the static CRT,
#                     see docs/COMPATIBILITY.md
set(MUB_MIN_WIN "win10" CACHE STRING "Minimum Windows version (win10 | vista | xp)")
set_property(CACHE MUB_MIN_WIN PROPERTY STRINGS win10 vista xp)

if(MUB_MIN_WIN STREQUAL "xp")
  set(MUB_WINNT 0x0501)
  set(MUB_LEGACY_TARGET ON)
elseif(MUB_MIN_WIN STREQUAL "vista")
  set(MUB_WINNT 0x0600)
  set(MUB_LEGACY_TARGET ON)
elseif(MUB_MIN_WIN STREQUAL "win10")
  set(MUB_WINNT 0x0A00)
  set(MUB_LEGACY_TARGET OFF)
else()
  message(FATAL_ERROR "MUB_MIN_WIN must be win10, vista or xp (got '${MUB_MIN_WIN}')")
endif()
message(STATUS "Minimum Windows target: ${MUB_MIN_WIN} (_WIN32_WINNT=${MUB_WINNT})")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MUB_BUILD_TESTS "Build the portable core unit tests" ON)
option(MUB_STATIC_RUNTIME "Link the C++ runtime statically (single-file distribution)" OFF)

if(MUB_LEGACY_TARGET)
  # The Windows XP toolset only supports the static C/C++ runtime, and a static
  # runtime also keeps the single-file promise on Vista/7.
  set(MUB_STATIC_RUNTIME ON)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                        _WIN32_WINNT=${MUB_WINNT} WINVER=${MUB_WINNT}
                        _CRT_SECURE_NO_WARNINGS)
if(MUB_MIN_WIN STREQUAL "xp")
  add_compile_definitions(MUB_TARGET_XP=1)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)
  add_compile_options("$<$<CONFIG:Release>:/O2>")
  if(MUB_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  if(MUB_MIN_WIN STREQUAL "xp" AND NOT "${CMAKE_GENERATOR_TOOLSET}" MATCHES "xp")
    message(WARNING
      "MUB_MIN_WIN=xp needs the Windows XP toolset: configure with -T v141_xp "
      "(Visual Studio 2017/2019 plus the 'C++ Windows XP Support' component). "
      "Without it the binary may import post-XP APIs. See docs/COMPATIBILITY.md.")
  endif()
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --------------------------------------------------------------------------
# Portable core (crypto, pairing, protocol, usbip wire inspection, settings)
# --------------------------------------------------------------------------
set(MUB_CORE_SOURCES
  src/core/base32.cpp
  src/core/logger.cpp
  src/core/pairing.cpp
  src/core/protocol.cpp
  src/core/os_support.cpp
  src/core/session_auth.cpp
  src/core/settings.cpp
  src/core/sha256.cpp
  src/core/strings.cpp
  src/core/usbip_wire.cpp
)

add_library(mub_core STATIC ${MUB_CORE_SOURCES})
target_include_directories(mub_core PUBLIC src)
if(MSVC)
  target_compile_options(mub_core PRIVATE /W4)
endif()

# --------------------------------------------------------------------------
# Application
# --------------------------------------------------------------------------
set(MUB_APP_SOURCES
  src/main.cpp
  src/client/client_mode.cpp
  src/client/usbip_cli.cpp
  src/net/channel.cpp
  src/server/server_mode.cpp
  src/server/usb_detect.cpp
  src/server/usbipd_cli.cpp
  src/ui/main_window.cpp
  src/ui/settings_dialog.cpp
  src/ui/theme.cpp
  src/win/crypto_win.cpp
  src/win/elevated.cpp
  src/win/net.cpp
  src/win/process.cpp
  src/win/tls.cpp
  src/win/wincompat.cpp
  src/ui/app.rc
)

add_executable(MiniUsbBridge WIN32 ${MUB_APP_SOURCES})
target_link_libraries(MiniUsbBridge PRIVATE mub_core
  ws2_32 secur32 crypt32 setupapi cfgmgr32 comctl32 shell32 ole32 advapi32 gdi32 user32 uuid
)
set_target_properties(MiniUsbBridge PROPERTIES OUTPUT_NAME "MiniUsbBridge")
# The manifest (comctl32 v6 + DPI awareness) travels as a resource. Attaching it
# through the sources list lets the MSVC toolchain embed it once; adding
# /MANIFESTINPUT on top of that makes mt.exe see the same manifest twice.
if(MSVC)
  target_sources(MiniUsbBridge PRIVATE src/ui/app.manifest)
endif()

# --------------------------------------------------------------------------
# Relay (own infrastructure for NAT traversal / encrypted fallback)
# --------------------------------------------------------------------------
add_executable(mubb-relay relay/relay.cpp)
target_link_libraries(mubb-relay PRIVATE ws2_32)

# A Windows XP binary must say so in its PE header, otherwise the XP loader
# refuses to start it ("not a valid Win32 application").
if(MUB_MIN_WIN STREQUAL "xp" AND MSVC)
  target_link_options(MiniUsbBridge PRIVATE "/SUBSYSTEM:WINDOWS,5.01")
  target_link_options(mubb-relay PRIVATE "/SUBSYSTEM:CONSOLE,5.01")
endif()

# --------------------------------------------------------------------------
# Tests (portable: can be built with any host compiler, no Windows needed)
# --------------------------------------------------------------------------
if(MUB_BUILD_TESTS)
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE mub_core)
  enable_testing()
  add_test(NAME core COMMAND test_core)
endif()

install(TARGETS MiniUsbBridge mubb-relay RUNTIME DESTINATION .)
install(FILES appconfig.example.ini docs/QUICKSTART.md docs/TROUBLESHOOTING.md docs/NETWORKING.md
        docs/COMPATIBILITY.md DESTINATION .)
