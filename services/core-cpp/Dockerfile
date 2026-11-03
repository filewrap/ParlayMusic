# core-cpp image. Multi-stage: g++ 13 + CMake build, slim runtime.
# libtdjson.so is linked at BUILD TIME (Phase 2): the build stage
# copies phase4/tdlib-src/build/libtdjson.so.1.8.69 (the path the
# CMakeLists defaults to) and the runtime stage carries it in
# /usr/local/lib — the binary's RUNPATH covers $ORIGIN/../lib, and
# the binary lives in /usr/local/bin. There is no dlopen of
# libtdjson anywhere: the binary cannot start without the .so.
# Runtime needs: curl CLI (healthcheck) and ffmpeg (Opus pipeline
# + dispatcher). No Python: the voice bridge (voice_bridge.py,
# Telethon child) was DELETED in Phase 5 — ntgcalls is a native
# library linked in-process by the Go backend, and the assistant
# session lives in TD_SESSION_DIR via TDLib.
#
# Build context is the REPO ROOT (see docker-compose.yml).
FROM gcc:13-bookworm AS build
# Proxy support (Phase 7 A6): lets image builds run behind an HTTP(S)
# proxy (apt/pip/go/cargo all honour these env vars). No-op when no
# build args are passed. Values are build-time only, never ENV.
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG no_proxy
ARG NO_PROXY
RUN apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY services/core-cpp ./services/core-cpp
COPY phase4/tdlib-src/build/libtdjson.so.1.8.69 ./phase4/tdlib-src/build/libtdjson.so.1.8.69
RUN cmake -S services/core-cpp -B services/core-cpp/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build services/core-cpp/build --parallel

FROM debian:bookworm-slim
# Proxy support (Phase 7 A6): lets image builds run behind an HTTP(S)
# proxy (apt/pip/go/cargo all honour these env vars). No-op when no
# build args are passed. Values are build-time only, never ENV.
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG no_proxy
ARG NO_PROXY
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/services/core-cpp/build/parlay-core /usr/local/bin/parlay-core
COPY --from=build /src/phase4/tdlib-src/build/libtdjson.so.1.8.69 /usr/local/lib/libtdjson.so.1.8.69
ENV PORT=8080
ENV PARLAY_DATA_DIR=/data
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
  CMD curl -fsS http://localhost:8080/healthz || exit 1
CMD ["parlay-core"]
