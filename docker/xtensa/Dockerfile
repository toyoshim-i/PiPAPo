# =============================================================================
# ppap/xtensa — Docker image for Xtensa/ESP32-S3 target (xtensa_cc)
# =============================================================================
#
# Contains:
#   - ESP-IDF v5.4 (source + Python venv + Xtensa toolchain via install.sh)
#   - cmake + ninja
#   - dfu-util (USB DFU flashing)
#
# Build:
#   docker build -t ppap/xtensa docker/xtensa/
#
# Usage:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD:/ppap" -w /ppap ppap/xtensa \
#     bash -c "source /opt/ppap/esp-idf-env.sh && \
#       cd src/target/xtensa_cc && \
#       idf.py -B /ppap/build/xtensa_cc set-target esp32s3 && \
#       idf.py -B /ppap/build/xtensa_cc build"
# =============================================================================

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# -- Apt packages ------------------------------------------------------------

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake ninja-build \
    git python3 python3-venv python3-pip ca-certificates wget \
    libffi-dev \
    dfu-util \
    flex bison gperf \
    libusb-1.0-0-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# -- ESP-IDF v5.4 -----------------------------------------------------------

RUN git clone --depth 1 -b v5.4 --recurse-submodules --shallow-submodules \
      https://github.com/espressif/esp-idf.git /opt/ppap/src/esp-idf

ENV IDF_PATH=/opt/ppap/src/esp-idf
ENV IDF_TOOLS_PATH=/opt/ppap/xtensa-tools

# Install Xtensa toolchain + Python venv for esp32s3
RUN /opt/ppap/src/esp-idf/install.sh esp32s3

# Create a convenience script to source the ESP-IDF environment
RUN echo '#!/bin/bash' > /opt/ppap/esp-idf-env.sh && \
    echo "export IDF_PATH=/opt/ppap/src/esp-idf" >> /opt/ppap/esp-idf-env.sh && \
    echo "export IDF_TOOLS_PATH=/opt/ppap/xtensa-tools" >> /opt/ppap/esp-idf-env.sh && \
    echo 'source /opt/ppap/src/esp-idf/export.sh' >> /opt/ppap/esp-idf-env.sh && \
    chmod +x /opt/ppap/esp-idf-env.sh

WORKDIR /ppap
