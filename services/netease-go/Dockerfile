# netease-go image. Multi-stage: Go 1.23 build (stdlib only, no module
# downloads beyond the stdlib), slim runtime with curl (healthcheck).
FROM golang:1.23-bookworm AS build
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 go build -trimpath -o /out/parlay-netease ./cmd/parlay-netease

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /out/parlay-netease /usr/local/bin/parlay-netease
ENV PORT=8010
ENV PARLAY_DL_DIR=/data/netease
EXPOSE 8010
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8010/healthz || exit 1
CMD ["parlay-netease"]
