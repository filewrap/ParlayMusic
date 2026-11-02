# gui-js image. Node 24 (matches the sandbox toolchain), stdlib only —
# no npm install step (no dependencies; `dev`/`start` = node app/main.js).
# curl is installed for the compose healthcheck.
FROM node:24-slim
RUN apt-get update && apt-get install -y --no-install-recommends curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY package.json ./
COPY app ./app
ENV PORT=3000
EXPOSE 3000
HEALTHCHECK --interval=10s --timeout=5s --retries=5 --start-period=10s \
    CMD curl -fsS http://localhost:3000/healthz || exit 1
CMD ["node", "app/main.js"]
