# PulseStream
Reliable synthetic cardiac-device telemetry and clinic monitoring in C++20, built by extending the cloned [IoTSharp/DurableDeliveryQueue](https://github.com/IoTSharp/DurableDeliveryQueue).

PulseStream adds POSIX HTTP/TCP ingestion, authenticated device registration, PostgreSQL idempotency, concurrent processing, transmission tracking, clinic notification review, audit history, and an operational dashboard. It is an independent portfolio simulator with engineering relevant to Medtronic-style connectivity workflows. Synthetic identifiers and events only; no diagnosis, ECG interpretation, device control, or emergency alerting.

The upstream C queue is the actual durability engine. Its source, license, and Git history remain intact. See [source provenance](docs/PROVENANCE.md) and the [original README](UPSTREAM_README.md).

## Run

```sh
cp .env.example .env
docker compose up --build -d
```

Open http://localhost:8080. The demo clinic credential in `.env.example` is `synthetic-clinic-a-credential`. The operator credential is `synthetic-operator-credential`. These are local synthetic demonstration values.

Populate the running Docker stack with the C++ device simulator:

```sh
docker compose exec -e PULSE_SIM_CLINIC=clinic-a \
  -e PULSE_SIM_DEVICE_TOKEN=synthetic-device-credential telemetry \
  device_simulator --devices 10 --events 20 --concurrency 4 --scenario mixed --output /data/benchmark.json
```

The simulator prints its run ID. A device ID is `<run_id>-d0`. Enter that ID in the dashboard to review status/history. The mixed scenario includes all four event types and a two-part scheduled summary. Benchmarks use the default heartbeat scenario.

Native build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
set -a
source .env
set +a
./build/pulsestream
```

Install PostgreSQL/libpq and OpenSSL first, and set `PULSE_DATABASE_URL` to an existing database. The [runbook](docs/RUNBOOK.md) includes macOS build paths, environment variables, monitoring, and recovery guidance. Docker configuration is included; runtime Docker execution was unavailable on the development host.

## API example

Eight business endpoints are specified in [OpenAPI](docs/openapi.yaml). Device submissions use device credentials; review endpoints use clinic credentials; registration and metrics use the operator credential.

```sh
curl -sS http://localhost:8080/v1/devices \
  -H 'Authorization: Bearer synthetic-operator-credential' \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"synthetic-device-1","clinic_id":"clinic-a","token":"synthetic-device-credential","overdue_after_ms":60000}'

curl -sS http://localhost:8080/v1/events \
  -H 'Authorization: Bearer synthetic-device-credential' \
  -H 'Content-Type: application/json' \
  -d '{"device_id":"synthetic-device-1","event_id":"synthetic-diagnostic-1","schema_version":1,"timestamp_ms":1000,"sequence":1,"type":"diagnostic_event","diagnostic_flag":"synthetic_review_flag"}'

curl -sS http://localhost:8080/v1/notifications \
  -H 'Authorization: Bearer synthetic-clinic-a-credential'
```

A 202 is returned only after DDQ synchronizes the enqueue record. Processing is asynchronous. Retry identical event payloads and identities after an ambiguous network result. A conflicting identity or reused device sequence is rejected. Notification retrieval records delivery; acknowledgment is a separate POST. Out-of-order events retain their original timestamps and sequences.

| Method | Business endpoint |
|---|---|
| POST | `/v1/devices` |
| POST | `/v1/events` |
| GET | `/v1/devices/{device_id}/status` |
| GET | `/v1/devices/{device_id}/events` |
| GET | `/v1/transmissions` |
| GET | `/v1/notifications` |
| POST | `/v1/notifications/{notification_id}/ack` |
| GET | `/v1/audit` |

The dashboard, `/healthz`, and operator-protected `/metrics` are additional operational routes.

## Verification

```sh
export PULSE_TEST_DATABASE_URL=postgresql://user:password@127.0.0.1:5432/pulsestream_test
python3 tests/integration.py
python3 tests/crash_matrix.py --rounds 100
PULSE_BINARY=build/pulsestream python3 scripts/benchmark.py --devices 2000 --events 40 --concurrency 32
```

The implementation has 56 GoogleTest cases, the original DDQ C suite, 26 integration tests, and a 100-trial process crash campaign. The crash campaign checks event and notification uniqueness, original payloads, and queue drain after replay. See [recorded evidence](docs/evidence/), [benchmark report](docs/BENCHMARKS.md), and [requirements traceability](docs/TRACEABILITY.md).

The 10,000 accepted events/sec objective is a target and was not achieved in the recorded local benchmark. No power-cut or medical-device certification is claimed. PostgreSQL is required for identity validation during admission. The single-owner DDQ foundation synchronizes each state transition; group commit and distributed ingestion are future work.

Separate sanitizer builds:

```sh
cmake -S . -B build-asan -DPULSE_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -DPULSE_SANITIZER=thread -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure
```

CI is configured to run these configurations with PostgreSQL and upload evidence. The local ASan and TSan binaries compiled, but their runtimes timed out before test output on this macOS host; sanitizer verification is incomplete. [Architecture and failure model](docs/ARCHITECTURE.md) explains the delivery contract and bounds; [Medtronic alignment](docs/MEDTRONIC_ALIGNMENT.md) explains the portfolio context. License: BSD-3-Clause, retaining original attribution.
