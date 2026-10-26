FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates libpq-dev libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake -S . -B build-docker -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && cmake --build build-docker -j 4
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libpq5 libssl3t64 ca-certificates curl && rm -rf /var/lib/apt/lists/* && useradd --uid 10001 --create-home pulse && mkdir -p /data/wal && chown -R pulse:pulse /data
COPY --from=build /app/build-docker/pulsestream /usr/local/bin/pulsestream
COPY --from=build /app/build-docker/device_simulator /usr/local/bin/device_simulator
COPY db /app/db
COPY dashboard /app/dashboard
USER pulse
ENV PULSE_BIND=0.0.0.0 PULSE_WAL_DIR=/data/wal
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=5s CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1
CMD ["pulsestream"]
