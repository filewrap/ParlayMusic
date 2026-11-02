# audio-fx-rust image. Multi-stage: Rust build (std-only, zero crates —
# --offline works), slim runtime with ffmpeg (the service is a thin
# wrapper around ffmpeg filter chains) and curl (healthcheck).
FROM rust:1-bookworm AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release --offline

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/target/release/parlay-audio-fx /usr/local/bin/parlay-audio-fx
ENV PORT=8004
ENV PARLAY_DATA_DIR=/data/audio-fx
EXPOSE 8004
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8004/healthz || exit 1
CMD ["parlay-audio-fx"]
