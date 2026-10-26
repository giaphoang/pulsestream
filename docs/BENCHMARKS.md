# Performance and recovery evidence

The 10,000 accepted events/sec objective is not achieved. The final implementation preserves the foundation's fsync-per-record policy and reports measured acceptance throughput separately from asynchronous worker drain. Measurements are development-host observations, not product or medical performance claims.

Host: Apple M3 Pro, 11 logical CPUs, 19,327,352,832 bytes RAM, arm64 macOS 26.5, PostgreSQL 17.7 Homebrew. Release binaries use AppleClang 17, C++20, default Release optimization. Service and clients share the same host and loopback network. No TLS is included. The default load is synthetic heartbeat JSON, approximately 149–151 bytes before HTTP and WAL envelope overhead. The earlier runs use 1,000 registered devices, while the final sustained run uses 2,000; all use 32 simulator threads, eight HTTP workers, and four processing workers. The WAL permits 100,000 items and 64 segments of 8 MiB; each DDQ state append is fsynced, and PostgreSQL uses synchronous commits.

| Recorded run | Attempted | Accepted | Failed acceptance | Accepted/sec | p50 / p95 / p99 ms |
|---|---:|---:|---:|---:|---|
| Initial short run, 8.73 s | 10,000 | 10,000 | 0 | 1,145.36 | 26.96 / 34.54 / 39.67 |
| Before DB connection reuse, 76.50 s | 40,000 | 39,149 | 851 | 511.78 | 41.24 / 66.65 / 84.95 |
| After DB connection reuse, 41.73 s | 40,000 | 40,000 | 0 | 958.57 | 31.51 / 61.30 / 66.59 |
| Final sustained run, 162.06 s | 80,000 | 80,000 | 0 | 493.66 | 61.86 / 124.26 / 130.91 |

These snapshots are `evidence/benchmark.json`, `evidence/prepool-sustained-benchmark.json`, `evidence/pooled-benchmark.json`, and `evidence/sustained-benchmark.json`. Independent client manifests verified zero missing acknowledged identities in the latter three runs. The final sustained report includes hardware, compiler, source hashes, status counts, latency percentiles, event size, processing settings, and a compressed acceptance manifest. All 80,000 acknowledged identities were found in PostgreSQL, with no duplicate event rows and no client retries. Its exact values are authoritative over this rounded table. Throughput declines with the larger retained history; no claim of constant throughput or 10,000/sec is made.

Latency covers the request through durable acceptance, including client retry delays, and excludes device registration and worker drain. Percentiles describe successful submissions, not requests that exhaust retries. A simulator failure is an event whose acceptance was not observed within five attempts; it is not automatically evidence that an acknowledged event was lost. Reports distinguish failed submissions, durable queue acceptance, committed database counts, and missing identities from the client's observed acceptance ledger.

Response sampling in `evidence/diagnostic-benchmark.json` exposed database connection failures during per-request connection creation. The implementation now caches one PostgreSQL session per bounded HTTP thread and reconnects broken sessions. The 40,000-event rerun had no transport errors, retries, rejected requests, missing acknowledged identities, or duplicate records. This resolves the observed connection-churn problem. It does not establish the 10,000/sec target. Serialized WAL synchronization, serialized admission, multiple database round trips, retained queue history, and one HTTP connection per request remain potential scaling limits; attribution among those requires profiling.

An earlier 100,000-attempt overload exploration is preserved in `evidence/overload-benchmark.json`. It had 809 failed submissions, and the original harness stopped before independent database verification. It is explicitly incomplete evidence. It also overlapped sanitizer startup probes, so it should not be compared as an isolated throughput run. The harness now saves failure results and verifies client-acknowledged identities even when some submissions fail.

Reproduce against a disposable PostgreSQL database:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
export PULSE_TEST_DATABASE_URL=postgresql://user:password@127.0.0.1:5432/pulsestream_test
PULSE_BINARY=build-release/pulsestream python3 scripts/benchmark.py \
  --devices 2000 --events 40 --concurrency 32 \
  --output docs/evidence/sustained-benchmark.json
```

The benchmark creates a unique database schema, runs the real server and C++ simulator, waits for queue ACK drain, loads the client's acceptance identities into a verification table, and checks every identity against committed events. It removes only its schema and temporary WAL directory afterward. Raw JSONL ledgers are ignored; the final report includes a compressed ledger and its uncompressed SHA-256.

`evidence/crash-report.json` records 100 process failures distributed among before-WAL, after-WAL, after-claim, after-database-commit, after-queue-ACK, and SIGKILL-after-client-response scenarios. All recovered event payloads and derived notification counts are checked, as is queue drain before the next trial. The report distinguishes observed client acknowledgments from trials that crash before a response. Before-WAL trials verify absence before resubmitting an identical reservation. Recovery timing covers startup/readiness, event visibility, idempotency verification, and queue drain, not just WAL parsing. Test leases are 100 ms; ordinary operation uses 30 seconds, so these timings do not predict production lease recovery. The filesystem and PostgreSQL remain intact; no hardware power-loss claim is made.

Normal local verification passes 56 GoogleTest cases, the original C test suite, and 26 integration tests. The browser smoke test verifies clinic status, notification delivery/review, metrics, isolation, and mobile viewport behavior. Docker Compose configuration parses, but the host Docker daemon was unavailable, so the container image/runtime was not executed. ASan/UBSan and TSan builds compile; instrumented executables time out before test output on this host, including the upstream C executable. Sanitizer validation remains incomplete. CI contains separate Linux sanitizer jobs; their presence is not a claim that a remote run has passed.
