# Toolchain image for cross-compiling GTA:SA ASI plugins (plugin-sdk, clang -> i686 MinGW).
# Mirrors plugin-sdk's own Linux clang CI (.github/workflows/Build_Plugin_GTA_SA_clang.yml).
FROM ubuntu:24.04

ARG LLVM_VERSION=23
ARG PREMAKE_VERSION=5.0.0-beta8
ARG PLUGIN_SDK_COMMIT=aa332312d4426eb373f1b8da5ad0e4d56495a46f

ENV DEBIAN_FRONTEND=noninteractive
ENV PLUGIN_SDK_DIR=/opt/plugin-sdk
ENV CC=clang-${LLVM_VERSION}
ENV CXX=clang++-${LLVM_VERSION}

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gnupg lsb-release software-properties-common \
        git make g++-mingw-w64-i686 \
    && wget -q https://apt.llvm.org/llvm.sh -O /tmp/llvm.sh \
    && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

RUN curl -fsSL https://github.com/premake/premake-core/releases/download/v${PREMAKE_VERSION}/premake-${PREMAKE_VERSION}-linux.tar.gz \
        | tar -xz -C /usr/local/bin premake5 \
    && chmod +x /usr/local/bin/premake5

RUN git clone https://github.com/DK22Pac/plugin-sdk.git ${PLUGIN_SDK_DIR} \
    && git -C ${PLUGIN_SDK_DIR} checkout ${PLUGIN_SDK_COMMIT} \
    && rm -rf ${PLUGIN_SDK_DIR}/.git

WORKDIR ${PLUGIN_SDK_DIR}
RUN premake5 gmake --file=tools/premake/premake5.lua \
    && make config=release CXX=${CXX} CC=${CC} AR=ar -C plugin_sa -j$(nproc) \
    && ls -la output/lib

COPY tools/build.sh /usr/local/bin/build-plugin
RUN sed -i 's/\r$//' /usr/local/bin/build-plugin && chmod +x /usr/local/bin/build-plugin

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/build-plugin"]
