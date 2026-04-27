# Tiny-Shell-Next-gen

# TinyShell NextGen (V3.1)

TinyShell NextGen is a high-performance, distributed execution runtime and secure orchestration platform implemented in C++20. Unlike traditional byte-stream shells (Bash/Zsh), TinyShell operates as a **Distributed Query Engine for OS Objects**, utilizing an immutable AST-based pipeline, hardware-accelerated kernel filtering, and a zero-trust security mesh.

---

## 1. Architecture

The system is architected into four decoupled planes to ensure horizontal scalability and fault tolerance:

### 📂 `common/` (The Data Plane & Shared Infrastructure)
*   **Immutable AST:** Defines the Abstract Syntax Tree and `TypedValue` variants that replace raw text pipes.
*   **SecureChannel:** A PQC-ready transport layer using **Kyber-768/X25519** handshakes and **AES-256-GCM** encryption.
*   **Serialization:** Implements binary AST serialization for efficient cross-node logic distribution.
*   **FastFileLoader:** High-performance I/O utilizing Linux `mmap` and C++20 `std::span`.

### 📂 `server/` (The Control Plane & Orchestrator)
*   **Execution Engine:** A C++20 coroutine-driven runtime executing JIT-compiled AST closures.
*   **Distributed Scheduler:** Manages the cluster via **Consistent Hashing** and **Load-Aware Round-Robin** placement.
*   **Security Mesh:** Contains the `CapabilityManager` (RBAC), `BpfFilterCompiler` (eBPF), and the hash-chained `ZkAuditTrail`.
*   **Query Planner:** Performs IR optimization passes, including **Predicate Pushdown** and **AST Minification**.

### 📂 `ui/tsh-dashboard/` (The Web Control Plane)
*   A **Next.js/React** application that serves as a centralized management console.
*   Connected via a non-blocking **C++ HTTP/JSON API Bridge** to provide real-time cluster telemetry.

### 📂 `client/` (The Remote Interface)
*   A secure CLI for dispatching pipelines to the orchestrator.
*   Includes **Keystroke Dynamics** and **TuiHolograph** for advanced session visualization.

---

## 2. Build Instructions

### Prerequisites
*   **Compiler:** GCC 11+ or Clang 13+ (C++20 requirement)
*   **Build System:** CMake 3.20+
*   **Dependencies:** OpenSSL 3.0+ (Development headers)
*   **OS:** Linux (Targeting kernel 5.15+ for eBPF/io_uring support)

### Compilation
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

---

## 3. Execution Guide

### Starting the Orchestrator (Server)
The server hosts the 60fps "Glass Cockpit" TUI and the API bridge.
```bash
./tsh_server
```

### Launching the Web Dashboard
```bash
cd ui/tsh-dashboard
npm install
npm run dev
```
Navigate to `http://localhost:3000` to view real-time risk HUD and node status.

### Connecting the Client
In a separate terminal:
```bash
./tsh_client
```

---

## 4. Implementation Details

### Execution Pipeline
1.  **Parsing:** `Parser.h` translates pipeline strings into an immutable `AstNode` graph.
2.  **Optimization:** `QueryPlanner.h` swaps `MAP -> FILTER` sequences to `FILTER -> MAP` (Predicate Pushdown).
3.  **JIT Compilation:** `AstJitCompiler.h` emits native machine closures from the AST for branchless execution.
4.  **Vectorization:** `VectorizedExecutor.h` processes data in batches using SIMD-ready SoA (Struct-of-Arrays) layouts.

### Security Layers
*   **Advanced Sandbox:** Utilizes **Seccomp-BPF** custom filters to kill processes attempting unauthorized `execve` or `socket` calls.
*   **Taint Tracker:** Statically analyzes the AST to block data flow from "Secret Sources" (Vault) to "Untrusted Sinks" (Network).
*   **ZK-Audit:** A cryptographically chained ledger where every command creates a Salted HMAC commitment, ensuring an unforgeable history.

### Distributed Design
*   **BFT Voting:** Critical commands are executed across 3 nodes with a majority-vote quorum to detect compromised workers.
*   **Consistent Hashing:** Pipelines are routed based on data-locality keys to maximize local cache hits.
*   **CRDT State:** Environment variables use **LWW-Register CRDTs** for eventual consistency across partitions.

---

## 5. Features
*   **60fps TUI Renderer:** Double-buffered incremental renderer with Braille sparklines.
*   **Unit-Aware Arithmetic:** AST-level validation of mathematical units (%, ms, bytes).
*   **Intent-Drift Detection:** Cognitive security that flags fatigue-based command streaks.
*   **Speculative Fan-Out:** Races identical ASTs across nodes to return the lowest latency result.
*   **Shannon Entropy Scoring:** Detects encrypted exfiltration or malware in data streams.

---

## 6. Developer Notes
*   **Ownership:** The project strictly follows RAII and strong ownership models using `std::unique_ptr` and `std::shared_ptr` for AST management.
*   **Extensions:** New pipeline operators should be added to `OpType` in `Ast.h` and implemented as a `JitOp` in `AstJitCompiler.h`.
*   **Observability:** All security events must be logged via `tui.log_message()` to ensure they appear in the thread-safe TUI audit feed.

---

## 7. Safety & Constraints
*   **Memory:** Every pipeline execution is gated by `ResourceLimiter` (cgroups v2).
*   **Zero-Trust:** No node is trusted; all ASTs must be cryptographically signed by the `AstSigner` before execution.
*   **Time-Travel:** Use `!rewind` to restore state snapshots via `StateSnapshot.h`.
