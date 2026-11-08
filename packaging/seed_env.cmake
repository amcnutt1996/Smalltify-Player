# Seed or refresh the app folder's .env at build time:
#   cmake -DSRC_DIR=<repo> -DDEST=<app>/.env -DSTAMP=<build>/env.stamp -DSEED=ON|OFF
#         -P seed_env.cmake
# SEED OFF (release builds) never reads the repo .env: DEST becomes a byte
# copy of .env.example when it is missing; an existing DEST is left alone.
# SEED ON (dev builds): a missing DEST starts from .env.example. Only the
# SPOTIFY_CLIENT_ID and SOLOIST_API_KEY lines of the repo .env are carried
# over; every other key there (the client secret above all) never reaches
# the build output. An existing DEST keeps every other line, and is
# rewritten only when those repo lines changed since the last seed (STAMP
# holds their hash), so a key saved in the app is not reverted by every
# rebuild. Values are never printed.
cmake_minimum_required(VERSION 3.20)

if(NOT SEED)
  get_filename_component(dest_dir "${DEST}" DIRECTORY)
  file(MAKE_DIRECTORY "${dest_dir}")
  if(NOT EXISTS "${DEST}")
    configure_file("${SRC_DIR}/.env.example" "${DEST}" COPYONLY)
  endif()
  file(CHMOD "${DEST}" PERMISSIONS OWNER_READ OWNER_WRITE)
  file(WRITE "${STAMP}" "off\n")
  return()
endif()

set(seeded "")
foreach(key SPOTIFY_CLIENT_ID SOLOIST_API_KEY)
  set(line_${key} "")
  if(EXISTS "${SRC_DIR}/.env")
    file(STRINGS "${SRC_DIR}/.env" found REGEX "^${key}=[A-Za-z0-9_-]+[ \t\r]*$")
    if(found)
      list(GET found -1 line) # the last assignment wins, as in the app
      string(STRIP "${line}" line)
      set(line_${key} "${line}")
      string(APPEND seeded "${line}\n")
    endif()
  endif()
endforeach()
string(SHA256 digest "${seeded}")

if(EXISTS "${DEST}")
  set(old "")
  if(EXISTS "${STAMP}")
    file(READ "${STAMP}" old)
  endif()
  if(old STREQUAL digest)
    return()
  endif()
  file(READ "${DEST}" text)
else()
  file(READ "${SRC_DIR}/.env.example" text)
endif()

# Replace a plain KEY=... line, or append one. The leading newline lets
# the pattern match the first line too.
set(text "\n${text}")
foreach(key SPOTIFY_CLIENT_ID SOLOIST_API_KEY)
  if(line_${key})
    string(REGEX MATCH "\n${key}=" has "${text}")
    if(has)
      string(REGEX REPLACE "\n${key}=[^\r\n]*" "\n${line_${key}}" text "${text}")
    else()
      if(NOT text MATCHES "\n$")
        string(APPEND text "\n")
      endif()
      string(APPEND text "${line_${key}}\n")
    endif()
  endif()
endforeach()
string(SUBSTRING "${text}" 1 -1 text)

file(WRITE "${DEST}" "${text}")
# It may hold the Soloist key: owner-only, like the app's own writes.
file(CHMOD "${DEST}" PERMISSIONS OWNER_READ OWNER_WRITE)
file(WRITE "${STAMP}" "${digest}")
