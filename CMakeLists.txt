cmake_minimum_required(VERSION 3.13)
project(printer_server C)

# ─────────────────────────────────────────────────────────────────────
#  Raspberry Pi Thermal-Printer Server
#
#  Build:   mkdir build && cd build && cmake .. && make -j$(nproc)
#  Install: sudo make install      (binary → /usr/local/bin/printer-server)
# ─────────────────────────────────────────────────────────────────────

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)            # we use strcasestr, strtok_r, getaddrinfo

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
  -Wno-implicit-fallthrough
  -D_GNU_SOURCE
)

# Enable mongoose features we rely on.
add_compile_definitions(
  MG_TLS=MG_TLS_OPENSSL
  MG_ENABLE_IPV6=1
  MG_ENABLE_LINES=1
)

# ── Find libcups ─────────────────────────────────────────────────────
# The pkg-config module name varies across distros:
#   Debian/Ubuntu: "cups"     (from libcups2-dev)
#   Fedora/Arch:   "cups"     (from cups-devel / libcups)
#   Some distros:  "libcups"
# We try each name, then fall back to find_library + find_path.
set(CUPS_FOUND FALSE)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(CUPS QUIET IMPORTED_TARGET cups)
  if(NOT CUPS_FOUND)
    pkg_check_modules(CUPS QUIET IMPORTED_TARGET libcups)
  endif()
  if(NOT CUPS_FOUND)
    pkg_check_modules(CUPS QUIET IMPORTED_TARGET libcups2)
  endif()
endif()

# Fall-back: find library and header directly (works even without .pc file).
if(NOT CUPS_FOUND)
  find_library(CUPS_LIB cups)
  find_path(CUPS_INC cups/cups.h)
  if(CUPS_LIB AND CUPS_INC)
    set(CUPS_FOUND TRUE)
    set(CUPS_LIBRARIES ${CUPS_LIB})
    set(CUPS_INCLUDE_DIRS ${CUPS_INC})
    message(STATUS "Found CUPS via find_library: ${CUPS_LIB}")
  else()
    message(FATAL_ERROR
      "CUPS development files not found.\n"
      "Install with:  sudo apt install libcups2-dev\n"
      "   or:         sudo dnf install cups-devel\n"
      "   or:         sudo pacman -S cups")
  endif()
endif()

# ── Find OpenSSL ─────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)

# ── Source files ─────────────────────────────────────────────────────
set(SRC
  src/main.c
  src/server.c

  src/routes/printers.c
  src/routes/print.c
  src/routes/discovery.c

  src/services/cups_service.c
  src/services/cert_service.c
  src/services/print_service.c

  src/helpers/response.c
  src/helpers/escpos.c
  src/helpers/image.c

  vendor/mongoose.c
  vendor/cJSON.c
)

add_executable(printer-server ${SRC})

target_include_directories(printer-server PRIVATE
  src
  vendor
)

# CUPS include dirs — may come from pkg-config or find_path.
if(CUPS_INCLUDE_DIRS)
  target_include_directories(printer-server PRIVATE ${CUPS_INCLUDE_DIRS})
endif()

target_link_libraries(printer-server PRIVATE
  m
  OpenSSL::SSL
  OpenSSL::Crypto
)

# CUPS link — use the pkg-config imported target if it exists, else raw lib path.
if(TARGET PkgConfig::CUPS)
  target_link_libraries(printer-server PRIVATE PkgConfig::CUPS)
elseif(CUPS_LIBRARIES)
  target_link_libraries(printer-server PRIVATE ${CUPS_LIBRARIES})
else()
  target_link_libraries(printer-server PRIVATE cups)
endif()

# POSIX threading — CUPS uses it internally.
find_package(Threads REQUIRED)
target_link_libraries(printer-server PRIVATE Threads::Threads)

# ── Install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS printer-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES config.json
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/printer-server
        RENAME config.json)

install(FILES systemd/printer-server.service
        DESTINATION /lib/systemd/system)

install(DIRECTORY www/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/printer-server/www)
