FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3t64 \
    libstdc++6 \
    curl \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --system --gid 10001 hps \
    && useradd --system --uid 10001 --gid hps --home-dir /app --shell /usr/sbin/nologin hps

WORKDIR /app

RUN mkdir -p /app/data && chown -R hps:hps /app

COPY --chown=hps:hps bin/high-performance-server \
    bin/file-send-process \
    bin/file-receive-process \
    ./
COPY --chown=hps:hps lib/ ./lib/

ENV PATH="/app:${PATH}" \
    SERVER_PORT=9090

EXPOSE 9090

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
  CMD curl -fsS http://127.0.0.1:9090/api/health || exit 1

STOPSIGNAL SIGTERM
USER hps

CMD ["./high-performance-server"]
