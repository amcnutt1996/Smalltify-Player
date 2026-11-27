# Build environment of the Windows x64 release (packaging/windows/release.sh
# builds and runs it; the build context is this folder). Ubuntu with
# MinGW-w64 gcc cross-compiling for x86_64-w64-mingw32, plus zlib and
# libcurl built here from source as static libraries into
# /opt/x86_64-w64-mingw32, the prefix mingw-w64-x86_64.cmake searches.
# libcurl uses Schannel, Windows' own TLS and certificate store, so no
# OpenSSL or CA bundle ships with the app.
#
# Sources are pinned by version and SHA-256. The sums were taken from
# tarballs whose PGP signatures checked out against the maintainers' keys
# (curl: Daniel Stenberg, 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2; zlib:
# Mark Adler, 5ED46A6721D365587791E2AA783FCD8E58BCAFBA, and zlib.net
# publishes the same sum). Update a version and its sum together.
#
# Ubuntu 24.04 rather than 26.04: 26.04's tar fails under Docker's x86-64
# emulation on Apple silicon (Rosetta), and both ship the same MinGW gcc 13.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# gcc-mingw-w64-x86-64-win32 is the compiler with the win32 thread model
# (the -posix one is not installed, so x86_64-w64-mingw32-gcc is always this
# one). git: CMake fetches Raven and the Vulkan headers. file, zip, unzip,
# osslsigncode: packaging, checks and Authenticode signing.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates cmake curl file gcc-mingw-w64-x86-64-win32 git make \
      ninja-build osslsigncode unzip xz-utils zip \
 && rm -rf /var/lib/apt/lists/*

COPY mingw-w64-x86_64.cmake /opt/x86_64-w64-mingw32/share/toolchain.cmake
WORKDIR /tmp/deps

# zlib: gzip and deflate for libcurl (the app's Accept-Encoding). Built with
# zlib's own MinGW makefile; only libz.a and its headers are installed.
ARG ZLIB_VERSION=1.4
ARG ZLIB_SHA256=02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584
RUN curl -fsSLo zlib.tar.gz \
      "https://github.com/madler/zlib/releases/download/v${ZLIB_VERSION}/zlib-${ZLIB_VERSION}.tar.gz" \
 && echo "${ZLIB_SHA256}  zlib.tar.gz" | sha256sum -c - \
 && tar xf zlib.tar.gz \
 && make -C "zlib-${ZLIB_VERSION}" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a \
 && install -Dm644 -t /opt/x86_64-w64-mingw32/include \
      "zlib-${ZLIB_VERSION}/zlib.h" "zlib-${ZLIB_VERSION}/zconf.h" \
 && install -Dm644 -t /opt/x86_64-w64-mingw32/lib "zlib-${ZLIB_VERSION}/libz.a" \
 && rm -rf /tmp/deps/*

# libcurl: static, Schannel, zlib, HTTP and HTTPS only. Everything that
# would need another library is off: no HTTP/2 (nghttp2), so the app's
# HTTP/2 request gets HTTP/1.1; no IDN, public suffix list, SSH, brotli or
# zstd. CMake's export (lib/cmake/CURL) gives the app CURL_STATICLIB and
# curl's Windows libraries (ws2_32, crypt32, secur32, bcrypt, ...).
ARG CURL_VERSION=8.23.0
ARG CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
RUN curl -fsSLo curl.tar.xz "https://curl.se/download/curl-${CURL_VERSION}.tar.xz" \
 && echo "${CURL_SHA256}  curl.tar.xz" | sha256sum -c - \
 && tar xf curl.tar.xz \
 && cmake -S "curl-${CURL_VERSION}" -B curl-build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=/opt/x86_64-w64-mingw32/share/toolchain.cmake \
      -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DCMAKE_INSTALL_PREFIX=/opt/x86_64-w64-mingw32 \
      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
      -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF \
      -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
      -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_PKGCONFIG=OFF \
      -DCURL_ZLIB=ON -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF \
      -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
      -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON \
      -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_IPFS=ON \
      -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON \
      -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_TELNET=ON \
      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_WEBSOCKETS=ON \
 && cmake --build curl-build \
 && cmake --install curl-build \
 && install -Dm644 "curl-${CURL_VERSION}/COPYING" \
      /opt/x86_64-w64-mingw32/share/licenses/curl-LICENSE.txt \
 && rm -rf /tmp/deps/*

# The static exe carries MinGW-w64's runtime: its copyright file (the
# winpthreads and CRT notices) ships with the app.
RUN install -Dm644 /usr/share/doc/mingw-w64-x86-64-dev/copyright \
      /opt/x86_64-w64-mingw32/share/licenses/winpthreads-LICENSE.txt

WORKDIR /src
