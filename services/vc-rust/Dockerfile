# vc-rust image. Multi-stage: Rust build (std-only, zero crates —
# --offline works), slim runtime with ffmpeg (Opus/RTP pipeline),
# python3 (ntg_offer.py local-offer helper) and yt-dlp (live resolve;
# $YT_DLP, unified name — see main.rs). NOTE: the old voice_bridge.py
# Telethon join client was deleted in Phase 5; vc-rust's voice.rs still
# references it (pending migration to the in-process model) — the join
# path currently reports the bridge missing, honestly.
FROM rust:1-bookworm AS build
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
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release --offline

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
    curl ffmpeg python3 python3-pip ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --break-system-packages --no-cache-dir yt-dlp
WORKDIR /app
COPY --from=build /src/target/release/parlay-vc-rust /usr/local/bin/parlay-vc-rust
COPY src/ntg_offer.py ./
ENV PORT=8003
ENV PARLAY_DATA_DIR=/data/vc-rust
ENV YT_DLP=/usr/local/bin/yt-dlp
EXPOSE 8003
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8003/healthz || exit 1
CMD ["parlay-vc-rust"]
