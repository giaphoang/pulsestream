# Requirements traceability

| ID | Implementation | Automated evidence |
|---|---|---|
| PS-01 Registered device authentication | Credential hash and device-to-clinic registry in `Service::route` | Integration 02–04; model validation cases |
| PS-02 Duplicate prevention | Immutable identity reservation; `(device_id,event_id)` and `(device_id,sequence)` uniqueness; idempotent transaction | Integration 05–08, 16; every crash trial checks events and derived notifications |
| PS-03 Preserve durably accepted events | Original DDQ enqueue synchronizes before 202 | Queue restart/ACK tests; integration 21; 100 process failure trials |
| PS-04 Replay unfinished processing | DDQ claim lease recovery; retry; DB commit before ACK | Queue retry/restart tests; crash stages after WAL, claim, commit, ACK; processing failure integration 18 |
| PS-05 Missing transmissions | Independent expected schedule, immutable multipart receipts, computed deadline state | Five tracking unit tests; integration 12–16 |
| PS-06 Clinic isolation | Clinic bearer lookup and SQL clinic predicates | Integration 10, 11, 17, 19, 20 |
| PS-07 Provenance | Full canonical payload plus original device ID, timestamp and sequence | Integration 09, 21; every crash trial compares original payload |
| PS-08 Bounded concurrency | Fixed HTTP/processing pools; bounded connections, body/header limits and DDQ capacity | Queue concurrency/capacity cases; framing cases; integration 07, 22; benchmark artifacts |

Delivery and staff review are tested separately by integration 17. Event receipt time is retained independently of device time. The test database failure fixture rejects event inserts via a PostgreSQL trigger, then verifies retry after removal; it does not claim a full PostgreSQL outage or disk failure test.

Target status: eight business endpoints implemented, 56 GoogleTest cases and one upstream C suite, 26 integration tests, and a 100-trial process crash campaign. `docs/evidence/` contains recorded benchmark and crash results. Throughput is an unmet 10,000 accepted events/sec target, not an inferred property of the architecture. See `BENCHMARKS.md` for measured workloads and practical limits. Integration 23 exercises all four event types through the actual C++ simulator; integration 24 verifies immutable pre-WAL reservations across a crash; integration 25 terminates only the fixture's database sessions and verifies reconnection; integration 26 verifies independent battery and connectivity receipt clocks.
