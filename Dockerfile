FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 \
    libstdc++6 \
    curl \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /data

WORKDIR /app

COPY bin/high-performance-server .
COPY lib/ lib/
COPY config.json .
COPY build/certs build/certs/
COPY data/ data/

EXPOSE 9090

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
  CMD curl -f http://localhost:9090/api/health || exit 1

CMD ["./high-performance-server", "--config", "config.json"]
