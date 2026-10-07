FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates clang-18 cmake curl file git zip \
    librsvg2-bin lld-18 llvm-18 ninja-build pkg-config python3 \
    python3-certifi python3-pip python3-pyelftools unzip xz-utils \
 && ln -sf /usr/lib/llvm-18/bin/llvm-config /usr/local/bin/llvm-config \
 && rm -rf /var/lib/apt/lists/*
