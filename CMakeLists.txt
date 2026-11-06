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
  add_test(NAME ${n} COMMAND ${n})
endforeach()

# Seed dist/.env from the repo's .env (never overwrite the user's copy).
if(EXISTS ${CMAKE_SOURCE_DIR}/.env AND NOT EXISTS ${SCP_DIST_DIR}/.env)
  file(COPY ${CMAKE_SOURCE_DIR}/.env DESTINATION ${SCP_DIST_DIR})
elseif(NOT EXISTS ${SCP_DIST_DIR}/.env)
  file(COPY_FILE ${CMAKE_SOURCE_DIR}/.env.example ${SCP_DIST_DIR}/.env)
endif()
