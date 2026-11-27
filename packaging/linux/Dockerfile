# Build image for the Linux x64 release (packaging/linux/release.sh, which
# builds it with --platform linux/amd64, also on arm64 hosts).
#
# Ubuntu 22.04 sets the floor: the binary needs glibc 2.35 or newer. libcurl
# and libX11 come from the distribution and are linked dynamically, so the
# user's machine needs libcurl4 and libx11-6 (any distribution ships them).
# Raven fetches the Vulkan headers itself and opens the Vulkan loader at run
# time; nothing Vulkan is installed here. git fetches Raven and the headers
# (CMake FetchContent).
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      gcc \
      git \
      libc6-dev \
      libcurl4-openssl-dev \
      libx11-dev \
      ninja-build \
 && rm -rf /var/lib/apt/lists/*

# release.sh runs the container as the invoking user, who has no home here.
ENV HOME=/tmp
WORKDIR /src
