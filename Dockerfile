# Brewer BLE HID Bridge - Build Environment
# Supports both ARM64 (Apple Silicon) and AMD64

FROM ubuntu:22.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    gperf \
    ccache \
    dfu-util \
    device-tree-compiler \
    wget \
    python3 \
    python3-dev \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    python3-venv \
    xz-utils \
    file \
    make \
    gcc \
    g++ \
    libmagic1 \
    && rm -rf /var/lib/apt/lists/*

# Install west and other Python dependencies
RUN pip3 install --no-cache-dir \
    west \
    pyelftools \
    pyyaml \
    packaging \
    progress \
    psutil \
    pyserial \
    requests \
    tabulate

# Set up Zephyr SDK
ENV ZEPHYR_SDK_VERSION=0.16.4

# Download and install full SDK (includes all toolchains)
RUN cd /tmp && \
    ARCH=$(uname -m) && \
    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then \
        HOST_ARCH="aarch64"; \
    else \
        HOST_ARCH="x86_64"; \
    fi && \
    echo "Downloading full SDK for ${HOST_ARCH}..." && \
    wget --progress=bar:force https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-${HOST_ARCH}.tar.xz && \
    echo "Extracting SDK (this takes a while)..." && \
    tar -xf zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-${HOST_ARCH}.tar.xz -C /opt && \
    rm -f /tmp/*.tar.xz && \
    cd /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION} && \
    ./setup.sh -h -c && \
    echo "SDK installed to /opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}"

ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-${ZEPHYR_SDK_VERSION}

# Install nRF Connect SDK
ENV NCS_VERSION=v2.6.1
ENV NCS_BASE=/opt/ncs

RUN mkdir -p ${NCS_BASE} && \
    cd ${NCS_BASE} && \
    west init -m https://github.com/nrfconnect/sdk-nrf --mr ${NCS_VERSION} . && \
    west update --narrow -o=--depth=1 && \
    west zephyr-export

# Set environment variables
ENV ZEPHYR_BASE=${NCS_BASE}/zephyr
ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.16.4

# Create working directory
WORKDIR /app

# Default command
CMD ["west", "build", "-b", "nrf52840dk_nrf52840"]
