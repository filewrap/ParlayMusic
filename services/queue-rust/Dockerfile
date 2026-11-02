# queue-rust image. Multi-stage: Rust build (std-only, zero crates —
# --offline works), slim runtime with curl (healthcheck). State is a JSON
# file under $PARLAY_DATA_DIR; vote threshold via $PARLAY_VOTE_THRESHOLD
# (default 3).
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
COPY --from=build /src/target/release/parlay-queue /usr/local/bin/parlay-queue
ENV PORT=8006
ENV PARLAY_DATA_DIR=/data/queue
EXPOSE 8006
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8006/healthz || exit 1
CMD ["parlay-queue"]
