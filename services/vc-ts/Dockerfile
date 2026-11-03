# vc-ts image. Node 24 (matches the sandbox toolchain), stdlib only —
# no npm install step. yt-dlp is installed so YouTube resolve works in
# the container the same way it does in the sandbox (where the service
# uses services/downloaders/.venv/bin/yt-dlp).
FROM node:24-slim
# Proxy support (Phase 7 A6): lets image builds run behind an HTTP(S)
# proxy (apt/pip/go/cargo all honour these env vars). No-op when no
# build args are passed. Values are build-time only, never ENV.
ARG http_proxy
ARG https_proxy
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG no_proxy
ARG NO_PROXY
RUN apt-get update && apt-get install -y --no-install-recommends python3 python3-pip curl \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --break-system-packages --no-cache-dir yt-dlp
WORKDIR /app
COPY package.json ./
COPY src ./src
ENV PORT=8002
ENV YT_DLP=/usr/local/bin/yt-dlp
EXPOSE 8002
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8002/healthz || exit 1
CMD ["node", "src/main.js"]
