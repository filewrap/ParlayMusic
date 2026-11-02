# downloaders image. Python 3.12 slim + ffmpeg (mp3 extraction) + yt-dlp
# (pip install from requirements.txt). curl is installed for the compose
# healthcheck. Downloads land in $PARLAY_DL_DIR (/data/downloads under
# compose). Soulseek is NOT in this image: it needs the separate slskd
# daemon (see docker-compose.yml); this service only talks to it over
# $SLSKD_URL once Worker D wires the client.
FROM python:3.12-slim
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
