# TINYSHELL NEXTGEN (V3.0) - DISTRIBUTED SYSTEM RE-ARCHITECTURE

## PHASE 1: SYSTEM UNDERSTANDING

### 1.1 Core Architecture Reconstruction
TinyShell NextGen has evolved from a local process executer to a distributed orchestration engine.
- **Control Flow:** `Server.cpp` accepts TCP connections, parses strings into an AST (`Parser.h`), validates structure (`PipelineValidator.h`), checks authorization (`CapabilityManager.h`), optimizes (`QueryPlanner.h`), and schedules (`JobScheduler.h`).
- **Data Flow:** ASTs are serialized (`AstSerializer.h`), signed (`AstSigner.h`), and pushed to remote workers (`DistributedOrchestrator.h`). Execution runs via C++20 coroutines (`Task.h`), streaming typed records (`Pipeline.h`) and applying BPF/cgroup isolation (`AdvancedSandbox.h`, `ResourceLimiter.h`).

### 1.2 ASCII Diagrams

**Current Execution Pipeline:**
```text
[Client] --> (Raw String) --> [Parser] --> AST DAG
                                            |
                                            v
[CapMan] <--- (Require) --- [Validator & Planner]
                                            |
                                            v
                              [AstSerializer & Signer]
                                            |
                                            v
[JobScheduler] ==(TCP/PQC)==> [Remote Worker Node]
                                    |
                                    +-> [AdvancedSandbox] (BPF)
                                    +-> [ResourceLimiter] (Cgroups)
                                    +-> [TypedStream<T> Executor]
                                            |
[AuditLogger] <== (Telemetry) ============= +
[Client]      <== (Result Data) =========== +
```

---

## PHASE 2: PROBLEM ANALYSIS

**1. Scalability Bottleneck: Single Orchestrator / Round-Robin**
* *Impact:* `JobScheduler.h` uses a mutex-locked `vector` and simple modulo math. At 1000 nodes, lock contention on `scheduler_mutex` will throttle the system to <10k TPS.
* *Failure Scenario:* 500 concurrent client requests pile up on the `scheduler_mutex`, causing TCP backlogs and client timeouts.

**2. Architecture Flaw: Coupled Data/Control Plane**
* *Impact:* The `DistributedOrchestrator` returns the *data* (results) back through the master node.
* *Failure Scenario:* A distributed `grep` over 1TB of logs across 100 nodes attempts to route 50GB of matching output through the single Orchestrator's RAM, causing OOM kills.

**3. Security Gap: Static Keys & Lack of Identity Propagation**
* *Impact:* `AstSigner.h` uses a hardcoded/static key. Capabilities checked on the master node do not robustly propagate to workers.
* *Failure Scenario:* Key leakage allows a compromised worker to forge ASTs and execute arbitrary BPF-bypassing graphs on other nodes.

---

## PHASE 3: TARGET ARCHITECTURE

**Separation of Planes:**
1. **Control Plane (CP):** Handles parsing, capability resolution, scheduling, and cluster state (Raft).
2. **Data Plane (DP):** Handles the actual `TypedStream<T>` execution and peer-to-peer data shuffling (Apache Arrow Flight).

**Redefined Boundaries:**
* **CP <-> DP API:** gRPC/mTLS. CP sends signed `ExecutionPlan` (AST + Token).
* **DP <-> DP API:** Zero-copy binary streams over TCP/QUIC.

---

## PHASE 4: CORE SYSTEM DESIGN

### 4.1 Execution Engine
* *Architecture:* Replace interpreted AST execution with a vectorized (SIMD) execution engine.
* *Data Structures:* `ColumnarBatch` (Arrow format) instead of `std::vector<ProcessRecord>`. Allows bulk AVX-512 operations.

### 4.2 Distributed Scheduler
* *Architecture:* Two-level scheduling (Omega/Borg style). A central allocator assigns *resources* to nodes; a localized task scheduler pulls *tasks* from a distributed queue.
* *Algorithms:* Consistent Hashing for data-locality.

### 4.3 Cluster State Management
* *Architecture:* Embedded Raft KV store (like etcd) for node health, topology, and capability revocation lists.

### 4.4 Security Model (End-to-End)
* *Architecture:* SPIFFE/SPIRE for mutual TLS. ASTs are packed with a cryptographic Macaroon (bearing specific capabilities like `cap_exec_memfd`) signed by the CP. Worker validates the Macaroon before execution.

---

## PHASE 5: SCALABILITY DESIGN (1 to 1000 Nodes)

* **Load Distribution:** Shift from Push (Orchestrator fans out) to Pull (Workers claim tasks from a distributed lock-free work-stealing queue).
* **State Sync:** Gossip Protocol (e.g., SWIM) replaces direct master-worker heartbeats to scale cluster state O(log N) instead of O(N^2).
* **Network Bottlenecks:** Intermediate reduction. Workers stream partial results to "Aggregator Nodes" in a tree topology rather than all 1000 workers sending data to the master.

---

## PHASE 6: FAILURE & RESILIENCE

* **Node Failure:** Orchestrator detects Gossip failure. Re-queues the AST DAG fragment.
* **Partial Execution:** Execution graphs are strictly **Idempotent**. Side-effecting nodes (e.g., `kill()`) require a Two-Phase Commit (2PC) coordinated by the CP.
* **Scheduler Crash:** Raft consensus promotes a standby CP node in < 500ms.

---

## PHASE 7: PERFORMANCE MODEL

* **Throughput Target:** 100,000 pipelines/sec across cluster.
* **Optimization Strategy:**
    1. Replace `std::vector<T>` copies with `std::span<T>` over memory-mapped files (`FastFileLoader`).
    2. Replace `std::variant` in hot loops with structured arrays (Struct-of-Arrays).
    3. `io_uring` for all socket reads in `Server.cpp`.

---

## PHASE 8: SECURITY MODEL (DEEP)

* **Trust Boundaries:** The Orchestrator is the root of trust. Workers are zero-trust.
* **Capability Propagation:**
  ```cpp
  struct ExecutionToken {
      std::string spiffe_id;
      std::vector<std::string> capabilities;
      uint64_t exp;
      std::string hmac_signature;
  };
  ```
  This token is injected into the AST before `AstSerializer::serialize`. The receiving worker's `CapMan` validates the token locally before initializing `AdvancedSandbox`.

---

## PHASE 9: IMPLEMENTATION MAPPING

**Refactoring Strategy:**
1. `server/Server.cpp` -> Split into `cp/Orchestrator.cpp` and `dp/WorkerNode.cpp`.
2. `server/Pipeline.h` -> Rewrite to `dp/VectorizedExecutor.h` using Columnar batches.
3. `server/JobScheduler.h` -> Replace with `cp/WorkQueue.h` and `cp/ConsistentHash.h`.
4. `common/AstSigner.h` -> Upgrade to `security/IdentityManager.h` (JWT/Macaroons).

---

## PHASE 10: TRADE-OFF ANALYSIS

1. **C++ Exceptions vs `std::expected<T, E>`**
   * *Choice:* Move to `std::expected` (C++23) or `absl::expected`.
   * *Why:* Exceptions across coroutine boundaries are extremely difficult to trace and cause severe performance degradation on failure.
2. **Push vs Pull Scheduling**
   * *Choice:* Pull (Work-stealing).
   * *Downside:* Increased latency for cold-start jobs compared to direct Push routing, but absolutely required for 1000+ node stability.

---

## PHASE 11: FINAL VALIDATION

**Top 5 Architectural Risks:**
1. Distributed split-brain during Raft network partitions leading to duplicate side-effects (e.g., killing a process twice).
2. Memory leaks in the C++20 coroutine promise/handle lifecycle under severe load.
3. Seccomp-BPF filters blocking legitimate required libc operations introduced in future updates.
4. Arrow Flight data-plane exhaustion (network switch saturation).
5. AST deserialization vulnerabilities (buffer overflows in `AstSerializer.cpp`).

**What Will Break First At Scale:**
The `TuiDashboard.h`. Updating the terminal at 60fps with telemetry from 1000 nodes will overwhelm the stdout buffer and freeze the orchestrator thread. Must be moved to an external metrics sink (Prometheus).

**What Must Be Rewritten Later:**
`TcpTransport`. Posix sockets must be replaced with `io_uring` or `eBPF` socket maps for true high-throughput DP networking.
