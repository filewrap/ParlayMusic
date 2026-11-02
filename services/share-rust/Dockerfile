# share-rust image. Multi-stage: Rust build (std-only, zero crates —
# --offline works), slim runtime with curl (healthcheck). State is a JSON
# file under $PARLAY_DATA_DIR (no SQLite — the old compose DATABASE_URL
# was dead config, removed).
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
COPY --from=build /src/target/release/parlay-share /usr/local/bin/parlay-share
ENV PORT=8005
ENV PARLAY_DATA_DIR=/data/share
EXPOSE 8005
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8005/healthz || exit 1
CMD ["parlay-share"]
