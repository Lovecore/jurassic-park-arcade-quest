# Build box64 with box32 (i386 guests on AArch64) for the Q0 perf probe.
# Output: /out/box64 inside the image; copy with docker create/cp.
FROM --platform=linux/arm64 debian:bookworm-slim AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    git ca-certificates cmake make gcc g++ python3 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/ptitSeb/box64 /src
WORKDIR /src/build
RUN cmake .. -DARM_DYNAREC=ON -DBOX32=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j8
RUN mkdir -p /out && cp box64 /out/ && git -C /src log -1 --format='%H %cd' > /out/VERSION
