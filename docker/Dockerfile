FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       make \
       pkg-config \
       libsdl2-dev \
       libsdl2-image-dev \
       libsdl2-ttf-dev \
       libcurl4-openssl-dev \
       libjson-c-dev \
       python3 \
       python3-pil \
       xvfb \
       x11vnc \
       openbox \
       novnc \
       websockify \
       x11-utils \
       fonts-dejavu-core \
       ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . /app
RUN chmod +x /app/docker/entrypoint.sh && make

EXPOSE 6080 8080

CMD ["/app/docker/entrypoint.sh"]
