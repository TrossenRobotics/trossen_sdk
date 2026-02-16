FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ENV TROSSEN_ARM_VERSION=1.9.0

# Install build tools and SDK dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    git \
    wget \
    lsb-release \
    pkg-config \
    libopencv-dev \
    libprotobuf-dev \
    libssl-dev \
    protobuf-compiler \
    && rm -rf /var/lib/apt/lists/*

# Install libtrossen-arm
RUN curl -L https://github.com/TrossenRobotics/trossen_arm/archive/refs/tags/v$TROSSEN_ARM_VERSION.tar.gz -o trossen_arm.tar.gz && \
    tar -xzf trossen_arm.tar.gz && \
    cd trossen_arm-$TROSSEN_ARM_VERSION && \
    make install && \
    cd .. && \
    rm -rf trossen_arm-$TROSSEN_ARM_VERSION trossen_arm.tar.gz

# Install Arrow C++ from official repository
RUN wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libarrow-dev \
    libparquet-dev && \
    rm -rf /var/lib/apt/lists/* && \
    rm apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

# Build and install trossen_sdk from local source
WORKDIR /trossen_sdk_build
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY cmake/ ./cmake/
COPY examples/ ./examples/
COPY tests/ ./tests/
RUN mkdir build && cd build && \
    cmake -DTROSSEN_ENABLE_REALSENSE=OFF .. && \
    make -j$(nproc) trossen_sdk && \
    find . -name "libtrossen_sdk.so" -exec cp {} /usr/local/lib/ \; && \
    cp -r ../include/trossen_sdk /usr/local/include/ && \
    mkdir -p /usr/local/include/ftservo && \
    find _deps/scservo-src/src -name "*.h" -exec cp {} /usr/local/include/ftservo/ \; && \
    cp scservo-build/libSCServo.so /usr/local/lib/ && \
    cp -r _deps/foxglove-src/include/* /usr/local/include/ && \
    cp -r ../include/trossen_sdk/third_party /usr/local/include/trossen_sdk/ && \
    ldconfig

WORKDIR /app

# Copy backend source files
COPY apps/trossen_sdk_ui/backend/CMakeLists.txt .
COPY apps/trossen_sdk_ui/backend/main.cpp .
COPY apps/trossen_sdk_ui/backend/include/ ./include/
COPY apps/trossen_sdk_ui/backend/src/ ./src/

RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=America/New_York

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    curl \
    wget \
    lsb-release \
    libopencv-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Arrow C++ runtime libraries
RUN wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
    libarrow2200 \
    libparquet2200 && \
    rm -rf /var/lib/apt/lists/* && \
    rm apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

RUN ldconfig

WORKDIR /app

COPY --from=builder /app/build/trossen_backend .
COPY --from=builder /usr/local/lib/libtrossen_sdk.so /usr/local/lib/
COPY --from=builder /usr/local/lib/libSCServo.so /usr/local/lib/
COPY --from=builder /usr/lib/x86_64-linux-gnu/libopencv*.so.4.5d /usr/lib/x86_64-linux-gnu/

COPY apps/trossen_sdk_ui/backend/data.json .

RUN ldconfig

EXPOSE 8080

CMD ["./trossen_backend"]
