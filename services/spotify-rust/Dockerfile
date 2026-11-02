# spotify-rust image. Multi-stage: Rust build (std-only, zero crates —
# --offline works), slim runtime with curl (healthcheck). Lyrics fall
# back to keyless LRCLib; Spotify Web API needs SPOTIFY_CLIENT_ID/SECRET.
FROM rust:1-bookworm AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release --offline

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/target/release/parlay-spotify /usr/local/bin/parlay-spotify
ENV PORT=8007
ENV PARLAY_DATA_DIR=/data/spotify
EXPOSE 8007
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8007/healthz || exit 1
CMD ["parlay-spotify"]
