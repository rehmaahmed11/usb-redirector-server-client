cmake_minimum_required(VERSION 3.20)
project(MiniUsbBridge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT WIN32)
  message(FATAL_ERROR "Mini USB Bridge targets Windows 10/11 only (Win32, Winsock, SChannel, SetupAPI).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MUB_BUILD_TESTS "Build the portable core unit tests" ON)
option(MUB_STATIC_RUNTIME "Link the C++ runtime statically (single-file distribution)" OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
                        _CRT_SECURE_NO_WARNINGS)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /MP)
  add_compile_options("$<$<CONFIG:Release>:/O2>")
  if(MUB_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
  src/ui/app.rc
)

add_executable(MiniUsbBridge WIN32 ${MUB_APP_SOURCES})
target_link_libraries(MiniUsbBridge PRIVATE mub_core
  ws2_32 secur32 crypt32 bcrypt setupapi cfgmgr32 comctl32 shell32 ole32 advapi32 gdi32 user32 uuid
)
set_target_properties(MiniUsbBridge PROPERTIES
  OUTPUT_NAME "MiniUsbBridge"
  LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/ui/app.manifest"
)
if(MSVC)
  target_sources(MiniUsbBridge PRIVATE src/ui/app.manifest)
endif()

# --------------------------------------------------------------------------
# Relay (own infrastructure for NAT traversal / encrypted fallback)
# --------------------------------------------------------------------------
add_executable(mubb-relay relay/relay.cpp)
target_link_libraries(mubb-relay PRIVATE ws2_32)

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
install(FILES appconfig.example.ini docs/QUICKSTART.md docs/TROUBLESHOOTING.md DESTINATION .)
