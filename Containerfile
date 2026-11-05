FROM ubuntu:24.04

ARG LLVM_VERSION=20
ARG PS5_SDK_VERSION=v0.44
ARG PS5_SDK_SHA256=4d06eab777bb135705b5a6816688c9132efb246093fe606ac488ab6cf65ca5b0

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      binutils ca-certificates git gnupg libdigest-sha-perl make python3 unzip wget \
    && wget -qO /tmp/apt-llvm-key.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && gpg --dearmor --yes -o /usr/share/keyrings/apt.llvm.org.gpg /tmp/apt-llvm-key.asc \
    && printf '%s\n' \
      "deb [signed-by=/usr/share/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/apt-llvm.list \
    && rm -f /tmp/apt-llvm-key.asc

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
      "clang-${LLVM_VERSION}" "lld-${LLVM_VERSION}" "llvm-${LLVM_VERSION}" \
    && test -x "/usr/lib/llvm-${LLVM_VERSION}/bin/llvm-config" \
    && rm -rf /var/lib/apt/lists/*

ENV LLVM_CONFIG=/usr/lib/llvm-20/bin/llvm-config
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

RUN wget -q \
      "https://github.com/ps5-payload-dev/sdk/releases/download/${PS5_SDK_VERSION}/ps5-payload-sdk.zip" \
      -O /tmp/ps5-payload-sdk.zip \
    && echo "${PS5_SDK_SHA256}  /tmp/ps5-payload-sdk.zip" | sha256sum -c - \
    && unzip -q /tmp/ps5-payload-sdk.zip -d /opt \
    && test -x "${PS5_PAYLOAD_SDK}/bin/prospero-clang" \
    && rm -f /tmp/ps5-payload-sdk.zip

WORKDIR /workspace
ENTRYPOINT ["make"]
CMD ["all"]
