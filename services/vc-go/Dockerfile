# vc-go image. Multi-stage: Go 1.23 build (mongo-driver module download
# needs network at build time — normal for a Docker build on a host with
# egress; go.sum pins it), slim runtime with yt-dlp so the resolver and
# live autoplay picks work in-container (B12).
FROM golang:1.23-bookworm AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY cmd ./cmd
COPY internal ./internal
RUN CGO_ENABLED=0 go build -trimpath -o /out/parlay-vc ./cmd/parlay-vc

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    curl python3 python3-pip ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --break-system-packages --no-cache-dir yt-dlp
WORKDIR /app
COPY --from=build /out/parlay-vc /usr/local/bin/parlay-vc
ENV PORT=8001
ENV PARLAY_DATA_DIR=/data/vc-go
ENV YT_DLP=/usr/local/bin/yt-dlp
EXPOSE 8001
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8001/healthz || exit 1
CMD ["parlay-vc"]
