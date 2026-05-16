# TinyShell Secure Extension Contract

TinyShell is a secure orchestration substrate, not an unrestricted remote shell.
Every new feature must remain possible to build, but no feature may bypass the
execution spine's security and recovery boundaries.

## Non-Bypassable Boundaries

Every feature must go through:

- privilege boundaries
- execution policy
- sandboxing or an equivalent isolation boundary
- audit logging
- capability isolation
- signed authorization
- recoverability paths
- observability events

Features that need privileged or kernel-aware behavior are allowed, but they
must be designed as controlled systems operations instead of root-equivalent
command execution.

## Required Design Pattern

New functionality should use this shape by default:

```text
client intent
-> server-side authorization and policy validation
-> signed immutable operation spec
-> assigned agent
-> capability-scoped runtime/helper
-> structured event stream
-> durable audit persistence
-> recovery journal or replay path
```

Privileged operations must be brokered. The network-facing agent should remain
unprivileged whenever possible, and privileged helpers should expose narrow IPC
operations rather than accepting arbitrary commands.

## Privileged Operation Rules

If a feature requires process inspection, namespaces, cgroups, mounts, network
introspection, filesystem orchestration, or kernel metrics:

- isolate the privileged code in a small broker/helper
- use Unix domain sockets or another local authenticated IPC mechanism
- validate peer identity with OS credentials where available
- expose explicit typed operations only
- require signed authorization from the control plane
- emit structured audit events before and after the operation
- make the operation idempotent or recoverable
- enforce rate limits and resource limits
- avoid broad capabilities such as `CAP_SYS_ADMIN` in long-lived processes

No privileged helper may become a generic command runner.

## Execution Rules

Remote execution must remain structured:

- no `system()`
- no `popen()`
- no `/bin/sh -c`
- no inherited ambient environment
- no GUI-side authorization
- no unsigned agent work
- no unbounded stdout/stderr
- no unaudited privileged action

Commands and operations must be represented as policy-validated specs with
explicit arguments, resource limits, isolation requirements, and lifecycle
events.

## Event And Audit Rules

Every meaningful state transition must produce a structured event. Durable audit
records must be distinct from debug logs and must include enough context to
reconstruct what happened:

- actor
- job or operation id
- signed spec identity
- target agent
- policy decision
- start and finish timestamps
- output digests where applicable
- final state
- failure reason

Production audit storage should be append-only from the application role's
perspective and should support replay after server restart.

## Feature Acceptance Checklist

A feature is architecturally acceptable only if the design answers:

- What is the trust boundary?
- Which process owns the privilege?
- Which exact capability is required?
- What policy authorizes the operation?
- What signed spec represents the operation?
- What sandbox or isolation boundary contains execution?
- What events are emitted?
- What is persisted durably?
- How does retry, crash recovery, and deduplication work?
- How is abuse rate-limited?
- What happens if the agent is compromised?

This contract is intentionally strict. It keeps TinyShell extensible without
allowing extensibility to collapse into unrestricted remote root access.
