cmake_minimum_required(VERSION 3.20)
project(smalltify_player VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()

# Portable output folder: everything the app needs sits next to the binary.
set(SMP_DIST_DIR ${CMAKE_BINARY_DIR}/dist)

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

# GUI font: a subset of Inter (SIL OFL 1.1; third_party/inter/subset.py
# makes it), compiled in as a byte array so the app needs no font file.
# Regenerated at configure time whenever the .ttf changes.
set(SMP_FONT ${CMAKE_SOURCE_DIR}/third_party/inter/Inter-Regular.subset.ttf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SMP_FONT})
file(READ ${SMP_FONT} SMP_FONT_BYTES HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SMP_FONT_BYTES "${SMP_FONT_BYTES}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n" SMP_FONT_BYTES "${SMP_FONT_BYTES}")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/ui_font.c CONTENT
  "/* Generated from third_party/inter/Inter-Regular.subset.ttf by CMakeLists.txt. */\nconst unsigned char ui_font_ttf[] = {\n@SMP_FONT_BYTES@};\nconst unsigned int ui_font_ttf_len = sizeof ui_font_ttf;\n"
  @ONLY)

# --- app -------------------------------------------------------------------
# smp_core = every module except main.c. Static lib: tests and the exe pull
# in only the objects they reference.
file(GLOB SMP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM SMP_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)
add_library(smp_core STATIC ${SMP_SOURCES} ${CMAKE_BINARY_DIR}/ui_font.c)
target_include_directories(smp_core PUBLIC src)
target_include_directories(smp_core PRIVATE third_party/inter)
target_link_libraries(smp_core PUBLIC raylib cjson CURL::libcurl Threads::Threads)
if(WIN32)
  # platform.c uses Vista+ APIs (SRW locks, condition variables); MinGW
  # headers default to an older target.
  target_compile_definitions(smp_core PRIVATE _WIN32_WINNT=0x0601)
  target_link_libraries(smp_core PUBLIC ws2_32 bcrypt)
endif()
if(APPLE)
  target_link_libraries(smp_core PUBLIC "-framework CoreServices") # quit Apple event
endif()
if(MSVC)
  target_compile_options(smp_core PRIVATE /W4)
else()
  target_compile_options(smp_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The app folder (.env, data/, soloist/; see plat_app_dir). On macOS the app
# is a bundle with the data in Contents/Resources: drag it to Applications,
# drag it to the Trash to uninstall everything.
add_executable(smalltify-player src/main.c)
target_link_libraries(smalltify-player PRIVATE smp_core)
if(APPLE)
  set(SMP_APP_NAME "Smalltify Player")
  set(SMP_EXE_NAME smalltify-player)
  set(SMP_BUNDLE "${SMP_DIST_DIR}/${SMP_APP_NAME}.app")
  set(SMP_APP_DIR "${SMP_BUNDLE}/Contents/Resources")
  # Assembled by hand rather than with MACOSX_BUNDLE, which names the bundle
  # after the executable; the app is "Smalltify Player.app" but the binary
  # keeps the CLI name.
  set_target_properties(smalltify-player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${SMP_BUNDLE}/Contents/MacOS")
  # CMake creates the output folder only at configure time; recreate it if
  # the bundle was deleted since.
  add_custom_command(TARGET smalltify-player PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SMP_BUNDLE}/Contents/MacOS"
    VERBATIM)
else()
  set(SMP_APP_DIR ${SMP_DIST_DIR})
  set_target_properties(smalltify-player PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${SMP_DIST_DIR})
endif()
if(WIN32)
  # GUI subsystem: launching never opens a console window. CLI commands
  # attach to the parent's console (plat_startup). MSVC keeps main() as the
  # entry point; MinGW's -mwindows (added by WIN32_EXECUTABLE) already does.
  set_target_properties(smalltify-player PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(smalltify-player PRIVATE /ENTRY:mainCRTStartup)
  endif()
endif()

# Seed <app>/.env at build time; see packaging/seed_env.cmake (only the
# client ID and Soloist key lines of the repo .env, never the secret; a
# rebuild refreshes them only when the repo .env changed).
set(SMP_ENV_STAMP ${CMAKE_BINARY_DIR}/env.stamp)
add_custom_target(smp_env ALL
  COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_SOURCE_DIR} -DDEST=${SMP_APP_DIR}/.env
          -DSTAMP=${SMP_ENV_STAMP} -P ${CMAKE_SOURCE_DIR}/packaging/seed_env.cmake
  BYPRODUCTS ${SMP_ENV_STAMP}
  VERBATIM)

# The font's license goes with the app, as the OFL asks.
set(SMP_FONT_LICENSE "${SMP_APP_DIR}/Inter-LICENSE.txt")
add_custom_command(OUTPUT ${SMP_FONT_LICENSE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SMP_APP_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/third_party/inter/LICENSE.txt
          ${SMP_FONT_LICENSE}
  DEPENDS ${CMAKE_SOURCE_DIR}/third_party/inter/LICENSE.txt
  VERBATIM)
add_custom_target(smp_font_license ALL DEPENDS ${SMP_FONT_LICENSE})

if(APPLE)
  find_program(SMP_CODESIGN codesign REQUIRED)
  configure_file(packaging/macos/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
  set(SMP_ICON ${CMAKE_SOURCE_DIR}/packaging/macos/AppIcon.icns)
  set(SMP_SIGN_STAMP ${CMAKE_BINARY_DIR}/bundle.stamp)
  # Ad-hoc signature (no developer ID; fine for a locally built app). Only
  # re-signed when the binary, plist, icon, font license or seeded .env
  # change: anything the app writes into Contents/Resources at runtime makes
  # `codesign --verify` report modified contents, which is expected.
  add_custom_command(OUTPUT ${SMP_SIGN_STAMP}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/Info.plist
            "${SMP_BUNDLE}/Contents/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SMP_ICON} "${SMP_APP_DIR}/AppIcon.icns"
    COMMAND ${SMP_CODESIGN} --force --sign - --options runtime "${SMP_BUNDLE}"
    COMMAND ${CMAKE_COMMAND} -E touch ${SMP_SIGN_STAMP}
    DEPENDS smalltify-player smp_env ${SMP_ENV_STAMP} ${CMAKE_BINARY_DIR}/Info.plist ${SMP_ICON}
            smp_font_license ${SMP_FONT_LICENSE}
    COMMENT "Signing ${SMP_APP_NAME}.app (ad hoc)"
    VERBATIM)
  add_custom_target(smp_bundle ALL DEPENDS ${SMP_SIGN_STAMP})
endif()

# --- tests: tests/test_<name>.c -> ctest target test_<name> -----------------
enable_testing()
file(GLOB SMP_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.c)
foreach(t ${SMP_TESTS})
  get_filename_component(n ${t} NAME_WE)
  add_executable(${n} ${t})
  target_link_libraries(${n} PRIVATE smp_core)
  # Release flags add -DNDEBUG; tests rely on assert().
  target_compile_options(${n} PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
  add_test(NAME ${n} COMMAND ${n})
endforeach()
