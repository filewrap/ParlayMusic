# backend-go image. Multi-stage: Go 1.26 + cgo build against the
# prebuilt libntgcalls (refs/ntgcalls/shared-output), slim runtime
# with ffmpeg + the yt-dlp daemon venv.
#
# Build context is the REPO ROOT (see docker-compose.yml): the cgo
# link bakes ${SRCDIR}/../../../../refs/ntgcalls/shared-output/lib
# as its rpath, so refs/ must sit at <context>/refs. At runtime the
# .so is also copied to /app/lib and LD_LIBRARY_PATH points there,
# so the image works even though the baked rpath is a build-time
# path. The backend finds the yt-dlp venv + worker script by
# walking up from its working directory (/app): both are copied
# under /app/scripts.
FROM golang:1.26-bookworm AS build
# Proxy support (Phase 7 A6): lets image builds run behind an HTTP(S)
# proxy (apt/pip/go/cargo all honour these env vars). No-op when no
# build args are passed. Values are build-time only, never ENV.
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG no_proxy
ARG NO_PROXY
WORKDIR /src
COPY services/backend-go/go.mod services/backend-go/go.sum ./services/backend-go/
RUN cd services/backend-go && go mod download
COPY services/backend-go ./services/backend-go
COPY refs/ntgcalls/shared-output ./refs/ntgcalls/shared-output
RUN cd services/backend-go \
    && CGO_ENABLED=1 go build -trimpath -o /out/parlay-backend ./cmd/backend

FROM debian:bookworm-slim
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG no_proxy
ARG NO_PROXY
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /out/parlay-backend /usr/local/bin/parlay-backend
COPY --from=build /src/refs/ntgcalls/shared-output/lib/libntgcalls.so /app/lib/libntgcalls.so
COPY scripts/yt_dlp_worker.py /app/scripts/yt_dlp_worker.py
RUN python3 -m venv /app/scripts/yt-dlp-venv \
    && /app/scripts/yt-dlp-venv/bin/pip install -q yt-dlp
ENV PARLAY_DATA_DIR=/data
ENV LD_LIBRARY_PATH=/app/lib
HEALTHCHECK --interval=5s --timeout=5s --retries=12 --start-period=10s \
  CMD test -S /data/parlay-backend.sock || exit 1
CMD ["parlay-backend"]
