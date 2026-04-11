FROM ubuntu:plucky

# ── Base toolchain: clang + meson + ninja ────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential python3 python3-pip ninja-build pkg-config \
        git wget ca-certificates gnupg lsb-release \
    && rm -rf /var/lib/apt/lists/*

# clang-23 (trunk) from apt.llvm.org — required for C23 support.
RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
        | gpg --dearmor -o /etc/apt/keyrings/llvm.gpg \
    && echo "deb [signed-by=/etc/apt/keyrings/llvm.gpg] \
        http://apt.llvm.org/plucky/ llvm-toolchain-plucky main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends clang-23 lld-23 \
    && ln -sf /usr/bin/clang-23   /usr/bin/clang \
    && rm -rf /var/lib/apt/lists/*

# meson >= 1.6.0 (repo version may be too old)
RUN pip3 install --break-system-packages 'meson>=1.6.0'

RUN clang --version && meson --version

ENV CC=clang
WORKDIR /src
