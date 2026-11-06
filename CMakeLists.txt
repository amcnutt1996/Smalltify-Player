cmake_minimum_required(VERSION 3.20)
project(spotify_c_player C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

# Portable output folder: everything the app needs sits next to the binary.
set(SCP_DIST_DIR ${CMAKE_BINARY_DIR}/dist)

# --- dependencies ----------------------------------------------------------
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Spotify album art is JPEG; raylib 5.5 leaves the JPEG decoder out by
# default. CUSTOMIZE_BUILD keeps every other option at its config.h default.
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
set(SUPPORT_FILEFORMAT_JPG ON CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(raylib)

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# --- app -------------------------------------------------------------------
# scp_core = every module except main.c. Static lib: tests and the exe pull
# in only the objects they reference.
file(GLOB SCP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM SCP_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)
add_library(scp_core STATIC ${SCP_SOURCES})
target_include_directories(scp_core PUBLIC src)
target_link_libraries(scp_core PUBLIC raylib cjson CURL::libcurl Threads::Threads)
if(MSVC)
  target_compile_options(scp_core PRIVATE /W4)
  target_link_libraries(scp_core PUBLIC ws2_32 bcrypt)
else()
  target_compile_options(scp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(spotify-c-player src/main.c)
target_link_libraries(spotify-c-player PRIVATE scp_core)
set_target_properties(spotify-c-player PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SCP_DIST_DIR})

# --- tests: tests/test_<name>.c -> ctest target test_<name> -----------------
enable_testing()
file(GLOB SCP_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.c)
foreach(t ${SCP_TESTS})
  get_filename_component(n ${t} NAME_WE)
  add_executable(${n} ${t})
  target_link_libraries(${n} PRIVATE scp_core)
  # Release flags add -DNDEBUG; tests rely on assert().
  target_compile_options(${n} PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
  add_test(NAME ${n} COMMAND ${n})
endforeach()

# Seed dist/.env once from .env.example (never overwrite the user's copy).
# For dev, only the SPOTIFY_CLIENT_ID line of the repo .env is carried over:
# any other key there (e.g. a client secret) must never reach dist/.
if(NOT EXISTS ${SCP_DIST_DIR}/.env)
  file(READ ${CMAKE_SOURCE_DIR}/.env.example SCP_ENV_TEXT)
  if(EXISTS ${CMAKE_SOURCE_DIR}/.env)
    file(STRINGS ${CMAKE_SOURCE_DIR}/.env SCP_ENV_ID REGEX "^SPOTIFY_CLIENT_ID=[A-Za-z0-9]+")
    if(SCP_ENV_ID)
      list(GET SCP_ENV_ID 0 SCP_ENV_ID)
      string(STRIP "${SCP_ENV_ID}" SCP_ENV_ID)
      string(REGEX REPLACE "SPOTIFY_CLIENT_ID=[^\n]*" "${SCP_ENV_ID}" SCP_ENV_TEXT "${SCP_ENV_TEXT}")
    endif()
  endif()
  file(WRITE ${SCP_DIST_DIR}/.env "${SCP_ENV_TEXT}")
endif()

# --- librespot: bundled playback engine (the only non-C component) ----------
# Built once with cargo; the custom command has no DEPENDS, so it only runs
# while the binary is missing (the first build takes several minutes).
# Features: rodio (cross-platform audio) + rustls with bundled webpki roots
# (pure Rust TLS on ring: no OpenSSL at build or run time, no mDNS stack).
option(SCP_BUILD_LIBRESPOT "Build the bundled librespot with cargo" ON)
if(SCP_BUILD_LIBRESPOT)
  find_program(SCP_CARGO cargo REQUIRED)
  set(SCP_LIBRESPOT_VERSION 0.8.0)
  set(SCP_LIBRESPOT_ROOT ${CMAKE_BINARY_DIR}/librespot)
  set(SCP_LIBRESPOT_BIN ${SCP_LIBRESPOT_ROOT}/bin/librespot${CMAKE_EXECUTABLE_SUFFIX})
  set(SCP_LIBRESPOT_DIST ${SCP_DIST_DIR}/librespot${CMAKE_EXECUTABLE_SUFFIX})
  add_custom_command(OUTPUT ${SCP_LIBRESPOT_BIN}
    COMMAND ${CMAKE_COMMAND} -E env
      CARGO_PROFILE_RELEASE_LTO=true
      CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
      CARGO_PROFILE_RELEASE_STRIP=true
      CARGO_PROFILE_RELEASE_PANIC=abort
      ${SCP_CARGO} install librespot --version ${SCP_LIBRESPOT_VERSION} --locked
        --root ${SCP_LIBRESPOT_ROOT}
        --target-dir ${SCP_LIBRESPOT_ROOT}/target
        --no-default-features --features rodio-backend,rustls-tls-webpki-roots
    COMMENT "Building librespot ${SCP_LIBRESPOT_VERSION} with cargo (slow the first time)"
    USES_TERMINAL
    VERBATIM)
  add_custom_command(OUTPUT ${SCP_LIBRESPOT_DIST}
    COMMAND ${CMAKE_COMMAND} -E copy ${SCP_LIBRESPOT_BIN} ${SCP_LIBRESPOT_DIST}
    DEPENDS ${SCP_LIBRESPOT_BIN}
    VERBATIM)
  add_custom_target(librespot ALL DEPENDS ${SCP_LIBRESPOT_DIST})
endif()
