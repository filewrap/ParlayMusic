# radio-js image. Node 24, stdlib only — no npm install step.
# ffmpeg is installed for ffprobe (stream reachability probing).
FROM node:24-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY package.json ./
COPY src ./src
ENV PORT=8008
EXPOSE 8008
CMD ["node", "src/main.js"]
