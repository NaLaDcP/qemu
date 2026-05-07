FROM node:20-slim AS builder

WORKDIR /app
COPY package*.json ./
RUN npm install

# ── Image finale ──────────────────────────────────────────────────────────────
FROM node:20-slim

WORKDIR /app
COPY --from=builder /app/node_modules ./node_modules
COPY gpio_server.js gpio_ui.html ./

EXPOSE 8080
CMD ["node", "gpio_server.js"]
