CREATE TABLE IF NOT EXISTS jobs (
    job_id TEXT PRIMARY KEY,
    user_id TEXT NOT NULL,
    command TEXT NOT NULL,
    args JSONB NOT NULL DEFAULT '[]'::jsonb,
    agent_id TEXT,
    state TEXT NOT NULL,
    next_event_sequence BIGINT NOT NULL DEFAULT 1,
    signed_spec BYTEA,
    key_id TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS job_events (
    event_id TEXT PRIMARY KEY,
    job_id TEXT NOT NULL REFERENCES jobs(job_id) ON DELETE CASCADE,
    sequence BIGINT NOT NULL,
    event_type TEXT NOT NULL,
    actor TEXT NOT NULL,
    agent_id TEXT,
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (job_id, sequence)
);

CREATE INDEX IF NOT EXISTS idx_job_events_job_sequence
    ON job_events(job_id, sequence);

CREATE INDEX IF NOT EXISTS idx_jobs_agent_state
    ON jobs(agent_id, state);

CREATE TABLE IF NOT EXISTS audit_events (
    audit_id TEXT PRIMARY KEY,
    job_id TEXT REFERENCES jobs(job_id) ON DELETE SET NULL,
    event_type TEXT NOT NULL,
    actor TEXT NOT NULL,
    agent_id TEXT,
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    prev_hash BYTEA,
    event_hash BYTEA,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_audit_events_job_created
    ON audit_events(job_id, created_at);

CREATE INDEX IF NOT EXISTS idx_audit_events_type_created
    ON audit_events(event_type, created_at);

CREATE TABLE IF NOT EXISTS agents (
    agent_id TEXT PRIMARY KEY,
    hostname TEXT NOT NULL,
    capabilities JSONB NOT NULL DEFAULT '[]'::jsonb,
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
