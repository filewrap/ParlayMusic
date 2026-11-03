# downloaders image. Python 3.12 slim + ffmpeg (mp3 extraction) + yt-dlp
# (pip install from requirements.txt). curl is installed for the compose
# healthcheck. Downloads land in $PARLAY_DL_DIR (/data/downloads under
# compose). Soulseek is NOT in this image: it needs the separate slskd
# daemon (see docker-compose.yml); this service only talks to it over
# $SLSKD_URL once Worker D wires the client.
FROM python:3.12-slim
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
    curl ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY src ./src
ENV PORT=8009
ENV PARLAY_DL_DIR=/data/downloads
EXPOSE 8009
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=15s \
    CMD curl -fsS http://localhost:8009/healthz || exit 1
CMD ["python", "-m", "src.main"]
