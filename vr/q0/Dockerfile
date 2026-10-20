# Q0 image: i386 Debian userland with the game's runtime deps, headless X.
# Build:  docker build --platform linux/386 -t jp2015-q0 vr/q0
# Run:    docker run --rm -it --platform linux/386 \
#           -v "$PWD/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]:/pm" \
#           jp2015-q0
FROM --platform=linux/386 debian:bookworm-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libsdl1.2debian \
    libgl1-mesa-dri libgl1-mesa-glx libglu1-mesa libglut3.12 mesa-utils \
    libtheora0 libogg0 libvorbisfile3 \
    libudev1 libx11-6 libxext6 \
    xvfb x11-apps x11-utils imagemagick \
    gcc libc6-dev make \
    procps file \
    && rm -rf /var/lib/apt/lists/*

# Software GL everywhere in this container.
ENV LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe

WORKDIR /pm
CMD ["/bin/bash"]
