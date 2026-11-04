# server-docker/Dockerfile -- a Multivoid dedicated server: Voices of the Void under Wine, headless.
#
# The image holds Wine, Xvfb and the start script, never the game: the game is not redistributable.
# The owner mounts or uploads their own game folder (UE4SS and Multivoid installed in it) to
# /home/container/VotV. See server-docker/README.md and docs/dedicated-server.md.
#
#   docker build -t multivoid-server server-docker
#   docker run -it --rm -p 47621:47621/udp -v "$PWD/data:/home/container" \
#       -e SAVE_SLOT=myworld -e SERVER_NAME="Our world" multivoid-server

FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# WineHQ's wine-staging (64-bit prefix; the i386 architecture is what winehq-staging depends on),
# Xvfb for a virtual display, Mesa for the software-RHI fallback, tini as PID 1.
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg procps tini xvfb xauth libgl1-mesa-dri cabextract \
 && mkdir -pm755 /etc/apt/keyrings \
 && curl -fsSL https://dl.winehq.org/wine-builds/winehq.key \
        | gpg --dearmor -o /etc/apt/keyrings/winehq-archive.key \
 && curl -fsSL -o /etc/apt/sources.list.d/winehq-bookworm.sources \
        https://dl.winehq.org/wine-builds/debian/dists/bookworm/winehq-bookworm.sources \
 && apt-get update \
 && apt-get install -y --install-recommends winehq-staging \
 && rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /entrypoint.sh
COPY multivoid-start.sh /usr/local/bin/multivoid-start
RUN chmod 0755 /entrypoint.sh /usr/local/bin/multivoid-start \
 && useradd -m -d /home/container -s /bin/bash container

# The Pterodactyl convention: user `container`, everything persistent under /home/container.
USER container
ENV USER=container \
    HOME=/home/container \
    WINEPREFIX=/home/container/.wine \
    WINEARCH=win64 \
    WINEDEBUG=-all
WORKDIR /home/container

# No -g: tini hands SIGTERM to the entrypoint only, which turns it into the server's `stop`.
ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
