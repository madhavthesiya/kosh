# Kosh in a container: build with the compiler, run without it.
#
#   docker build -t kosh .
#   docker run -d --name kosh -p 127.0.0.1:6379:6379 kosh --protected-mode no
#   redis-cli ping
#
# See "Run with Docker" in README.md for passwords and keeping the data.

# --- Build stage: compiler, CMake, sources. None of it ships. ---
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# RelWithDebInfo (-O2) is what every number in the README was measured with;
# strip then drops the debug symbols, not the optimisation.
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 && cmake --build build --target kosh-server -j"$(nproc)" \
 && strip build/kosh-server \
 && mkdir /data

# --- Run stage: glibc and libstdc++, no shell, no package manager. ---
# glibc, not Alpine's musl: every number in the README was measured with
# glibc's allocator, and the allocator already caused one stall (README,
# Challenge 15). musl's is a different allocator, not measured here.
FROM gcr.io/distroless/cc-debian12:nonroot
COPY --from=build /src/build/kosh-server /usr/local/bin/kosh-server
# The AOF lives in /data, owned by the image's unprivileged user (65532)
COPY --from=build --chown=65532:65532 /data /data
WORKDIR /data
VOLUME /data
USER 65532:65532
EXPOSE 6379
# A container's port is reached through Docker's network, so listen on every
# interface. Protected mode stays on: with no password, clients that aren't
# on this machine are refused until you pass a password file or
# --protected-mode no. Options given to docker run come after these, and the
# last one given wins.
ENTRYPOINT ["kosh-server", "--bind", "0.0.0.0"]
