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
# default. CUSTOMIZE_BUILD keeps every other option at its config.h default
# except the ones below, which this app never uses.
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
set(SUPPORT_FILEFORMAT_JPG ON CACHE BOOL "" FORCE)
foreach(opt
    SUPPORT_MODULE_RAUDIO SUPPORT_MODULE_RMODELS
    # F12 / Ctrl+F12 would write screenshotNNN.png / .gif into the working
    # directory, outside the portable app folder.
    SUPPORT_SCREEN_CAPTURE SUPPORT_GIF_RECORDING SUPPORT_AUTOMATION_EVENTS
    # Never set a target FPS (event waiting), so frame pacing is unused; on
    # Windows the timer option would raise the system timer to 1 ms for the
    # whole session, which costs battery.
    SUPPORT_PARTIALBUSY_WAIT_LOOP SUPPORT_WINMM_HIGHRES_TIMER
    SUPPORT_FILEFORMAT_GIF SUPPORT_FILEFORMAT_QOI SUPPORT_FILEFORMAT_DDS)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
# Drop unreferenced functions (most of raylib) from the executable.
if(APPLE)
  add_link_options(-Wl,-dead_strip)
elseif(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
  add_link_options(-Wl,--gc-sections)
endif()
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
if(WIN32)
  # platform.c uses Vista+ APIs (SRW locks, condition variables); MinGW
  # headers default to an older target.
  target_compile_definitions(scp_core PRIVATE _WIN32_WINNT=0x0601)
  target_link_libraries(scp_core PUBLIC ws2_32 bcrypt)
endif()
if(MSVC)
  target_compile_options(scp_core PRIVATE /W4)
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
# any other key there (a client secret, the SOLOIST_API_KEY) must never
# reach dist/, so the Soloist key stays the template's empty placeholder.
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
