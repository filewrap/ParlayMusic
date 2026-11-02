# core-cpp image. Multi-stage: g++ 13 + CMake build, slim runtime.
# Runtime needs: curl CLI (src/http.cpp shells to it) and ffmpeg (Opus
# pipeline + dispatcher execlp). No Python: the voice bridge
# (voice_bridge.py, Telethon child) was DELETED in Phase 5 (user order —
# ntgcalls is a native library linked in-process; the assistant session
# lives in TD_SESSION_DIR via TDLib). libtdjson.so is NOT baked in: the
# phase4 build lives at a host path (phase4/tdlib-src/build/libtdjson.so);
# pass TDJSON_LIB at runtime and mount the .so if the TDLib backend is
# wanted. The binary runs without it (voice reports unavailable honestly).
FROM gcc:13-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/parlay-core /usr/local/bin/parlay-core
ENV PORT=8080
ENV PARLAY_DATA_DIR=/data/core
EXPOSE 8080
# No HEALTHCHECK: the core serves no HTTP yet (A2 — GUI /api surface is
# a follow-up); compose therefore has no healthcheck for this service.
CMD ["parlay-core"]
