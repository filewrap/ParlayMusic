# radio-js image. Node 24, stdlib only — no npm install step.
# ffmpeg is installed for ffprobe (stream reachability probing).
FROM node:24-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY package.json ./
COPY src ./src
ENV PORT=8008
EXPOSE 8008
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:8008/healthz || exit 1
CMD ["node", "src/main.js"]
