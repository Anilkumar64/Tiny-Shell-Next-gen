# TinyShell NextGen — GUI

Modern Qt6 control panel for the TinyShell distributed shell orchestration platform.

## Screenshot overview

| Page | What it shows |
|---|---|
| **Dashboard** | Live KPI cards (commands OK/error, connections, queue depth, p99 latency) polled from `/metrics` every 3 s |
| **Terminal** | Execute allowed commands (`ps`, `uptime`, `who`, `df`) via the `/exec` HTTP API; history, coloured output |
| **Cluster** | Worker node table — health score, success/fail counters, add/remove nodes |
| **Jobs** | Submit AST jobs with Round-Robin, Speculative Fan-Out, or BFT routing; live queue ticker |
| **RBAC** | Role editor with permission checkboxes mirroring `RbacManager.h` |
| **Tenants** | Multi-tenant management — create tenants, assign users, edit resource quotas |
| **Audit Log** | Live-streaming event table with severity/outcome filters and free-text search |
| **Settings** | API URL + token configuration, connection test, env-var cheatsheet |

---

## File map

```
gui/
├── CMakeLists.txt       ← add_subdirectory(gui) in the parent CMakeLists
├── main_gui.cpp         ← QApplication entry point + splash screen
│
├── TshStyle.h           ← Dark-theme QSS stylesheet + colour palette
├── ApiClient.h          ← QNetworkAccessManager wrapper with Bearer auth
├── MetricCard.h         ← Reusable KPI card widget
│
├── MainWindow.h         ← QMainWindow: sidebar navigation + QStackedWidget
├── DashboardWidget.h    ← Metrics page
├── TerminalWidget.h     ← /exec terminal page
├── ClusterWidget.h      ← Cluster node management
├── JobWidget.h          ← Job scheduler
├── RbacWidget.h         ← RBAC roles & permissions editor
├── TenantWidget.h       ← Multi-tenant management
├── AuditWidget.h        ← Audit log viewer
└── ConfigWidget.h       ← API connection settings
```

---

## Build

### 1. Prerequisites

- **Qt 6.2+** (Qt 5.15 works — CMakeLists auto-detects)  
  Required modules: `Core Gui Widgets Network`  
  Optional: `Charts` (enables line charts on Dashboard)

- **CMake 3.20+**

- A C++20-capable compiler (GCC 11+, Clang 13+, MSVC 2022+)

### 2. Hook into the parent build

Add one line to `TinyShell/CMakeLists.txt`:

```cmake
add_subdirectory(gui)
```

Then build as usual:

```bash
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/Qt6
cmake --build build --target tsh_gui -j$(nproc)
```

### 3. Standalone build

```bash
cd gui
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/Qt6
cmake --build build -j$(nproc)
./build/tsh_gui
```

### 4. CLI flags

```
tsh_gui --url https://myserver:8080 --token my-secret-token
```

Flag | Default | Description
-----|---------|------------
`-u, --url` | `https://127.0.0.1:8080` | TinyShell HTTP API base URL
`-t, --token` | _(empty)_ | Bearer token (`TSH_API_TOKEN`)

---

## Connecting to a live server

```bash
# Start the TinyShell server (in TinyShell/server/)
export TSH_API_TOKEN=$(openssl rand -base64 32)
export TSH_ZK_SECRET=$(openssl rand -hex 32)
./tsh_server

# Start the GUI
./tsh_gui --token "$TSH_API_TOKEN"
```

The Dashboard and Terminal pages will immediately start polling `/health` and `/metrics`.  
The Cluster, RBAC, Tenant, and Job pages operate with in-process state until the backend exposes REST endpoints for those resources.

---

## Extending the GUI

Each page is a self-contained `QWidget` subclass in its own header file.  
To add a new page:

1. Create `gui/MyPageWidget.h` (follow the pattern of any existing widget).
2. `#include "MyPageWidget.h"` in `MainWindow.h`.
3. Add a `new MyPageWidget(m_stack)` + `m_stack->addWidget(...)` entry.
4. Add a nav button entry in the `navItems` list.
5. Add the header to `TSH_GUI_HEADERS` in `CMakeLists.txt`.

---

## Design system

| Token | Value | Used for |
|---|---|---|
| `BG_BASE` | `#0f1117` | Window background |
| `BG_SIDEBAR` | `#13151c` | Sidebar |
| `BG_CARD` | `#1c1f2b` | Cards / panels |
| `ACCENT` | `#00b4d8` | Active nav, buttons, highlights |
| `SUCCESS` | `#4ade80` | Healthy / OK states |
| `WARNING` | `#fbbf24` | Degraded / pending |
| `DANGER` | `#f87171` | Errors / failures |
| `TEXT_PRIMARY` | `#e8eaf0` | Body text |
| `TEXT_SECONDARY` | `#8891b0` | Labels / metadata |

All tokens are in `TshStyle.h` as `QColor` constants and baked into the single QSS string returned by `TshStyle::appStyleSheet()`.
