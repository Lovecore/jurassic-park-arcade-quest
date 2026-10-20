# Clean AArch64 host for kit tests: no qemu, no i386 libs. Only a headless X
# server + screenshot tools, standing in for the display the Quest app owns.
FROM --platform=linux/arm64 debian:bookworm-slim
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    xvfb x11-apps imagemagick procps && rm -rf /var/lib/apt/lists/*
