BEGIN;
CREATE TABLE IF NOT EXISTS clinics (
 clinic_id text PRIMARY KEY, token_hash text NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS devices (
 device_id text PRIMARY KEY, clinic_id text NOT NULL REFERENCES clinics,
 token_hash text NOT NULL, registered_ms bigint NOT NULL,
 last_seen_ms bigint, max_sequence bigint NOT NULL DEFAULT 0, battery_pct integer,
 overdue_after_ms bigint NOT NULL CHECK (overdue_after_ms BETWEEN 1000 AND 604800000)
);
ALTER TABLE devices ADD COLUMN IF NOT EXISTS battery_seen_ms bigint;
-- Admission identities reserve immutable payloads before WAL append. A reservation
-- alone never means accepted: a failed append can be retried with identical data.
CREATE TABLE IF NOT EXISTS event_identities (
 device_id text NOT NULL REFERENCES devices, event_id text NOT NULL,
 canonical text NOT NULL, sequence bigint NOT NULL,
 PRIMARY KEY(device_id,event_id), UNIQUE(device_id,sequence)
);
CREATE TABLE IF NOT EXISTS expected_transmissions (
 device_id text NOT NULL REFERENCES devices, transmission_id text NOT NULL,
 due_ms bigint NOT NULL, parts_total integer NOT NULL CHECK(parts_total BETWEEN 1 AND 1024),
 PRIMARY KEY(device_id,transmission_id)
);
CREATE TABLE IF NOT EXISTS events (
 device_id text NOT NULL REFERENCES devices, event_id text NOT NULL,
 clinic_id text NOT NULL REFERENCES clinics, sequence bigint NOT NULL,
 timestamp_ms bigint NOT NULL, received_ms bigint NOT NULL, type text NOT NULL,
 payload jsonb NOT NULL, PRIMARY KEY(device_id,event_id), UNIQUE(device_id,sequence)
);
CREATE INDEX IF NOT EXISTS events_clinic_time ON events(clinic_id,received_ms DESC);
CREATE TABLE IF NOT EXISTS transmission_parts (
 device_id text NOT NULL, transmission_id text NOT NULL, part_index integer NOT NULL,
 event_id text NOT NULL, received_ms bigint NOT NULL,
 PRIMARY KEY(device_id,transmission_id,part_index),
 FOREIGN KEY(device_id,transmission_id) REFERENCES expected_transmissions,
 FOREIGN KEY(device_id,event_id) REFERENCES events
);
CREATE TABLE IF NOT EXISTS notifications (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
 clinic_id text NOT NULL REFERENCES clinics, device_id text NOT NULL, event_id text NOT NULL,
 flag text NOT NULL, created_ms bigint NOT NULL, delivered_ms bigint, acknowledged_ms bigint,
 UNIQUE(device_id,event_id), FOREIGN KEY(device_id,event_id) REFERENCES events
);
CREATE TABLE IF NOT EXISTS audit (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, clinic_id text REFERENCES clinics,
 action text NOT NULL, subject text NOT NULL, occurred_ms bigint NOT NULL
);
CREATE INDEX IF NOT EXISTS audit_clinic_id ON audit(clinic_id,id DESC);
COMMIT;
