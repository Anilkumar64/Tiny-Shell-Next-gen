# TinyShell - Quick Start Guide

## Prerequisites

Install required dependencies:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y \
  build-essential cmake git \
  libssl-dev libgrpc-dev libprotobuf-dev protobuf-compiler \
  grpc-proto libgrpc++1-dev \
  postgresql postgresql-contrib postgresql-client \
  qt6-base-dev qt6-declarative-dev qt6-tools-dev \
  libasan6 libubsan1 ca-certificates

# Or for minimal build (without PostgreSQL/Qt):
sudo apt install -y \
  build-essential cmake git \
  libssl-dev libgrpc-dev libprotobuf-dev protobuf-compiler
```

## Setup

### 1. Initialize PostgreSQL Database

```bash
chmod +x ./setup-postgres.sh
./setup-postgres.sh
```

This script will:
- Check if PostgreSQL is running
- Create the `tsh` user if needed
- Create the `tinyshell` database
- Initialize the schema

If PostgreSQL isn't running, start it:

```bash
# System PostgreSQL
sudo systemctl start postgresql

# Or use Docker
docker run -d \
  --name tsh-postgres \
  -e POSTGRES_USER=tsh \
  -e POSTGRES_PASSWORD= \
  -e POSTGRES_DB=tinyshell \
  -p 5432:5432 \
  postgres:15
```

### 2. Build and Run

```bash
export TSH_API_TOKEN=$(openssl rand -base64 32)
export TSH_JOB_SIGNING_KEY=$(openssl rand -hex 32)
mkdir -p certs
openssl req -x509 -newkey rsa:4096 -keyout certs/server.key \
  -out certs/server.crt -days 365 -nodes -subj "/CN=tinyshell-server"
chmod +x ./start.sh
./start.sh
```

The script will:
1. Configure CMake with Release mode
2. Build all components (server, worker, GUI, spine services)
3. Start the worker, spine server, spine agent, and main server
4. Launch the GUI

## Architecture

- **Spine Server** (port 7443): gRPC-based job execution orchestrator
- **Spine Agent** (port 7444): Connects to spine server and executes jobs
- **Main Server** (port 4444): Legacy TinyShell server
- **Worker** (port 5555): Job execution worker
- **API/GUI** (port 8080): Web API and Qt GUI

## Environment Variables

Configure behavior by setting these before running `./start.sh`:

```bash
# PostgreSQL
export TSH_DB_HOST=127.0.0.1
export TSH_DB_PORT=5432
export TSH_DB_NAME=tinyshell
export TSH_DB_USER=tsh

# Spine Services
export TSH_SPINE_CONTROL_PORT=7443
export TSH_SPINE_AGENT_PORT=7444

# Job Signing (must be ≥32 bytes)
export TSH_JOB_SIGNING_KEY=$(openssl rand -hex 32)
export TSH_API_TOKEN=$(openssl rand -base64 32)
export TSH_ADMIN_TOKEN=$(openssl rand -base64 32)
export TSH_VIEWER_TOKEN=$(openssl rand -base64 32)
export TSH_TLS_CERT=certs/server.crt
export TSH_TLS_KEY=certs/server.key

# Server Ports
export TSH_PORT=4444
export TSH_API_PORT=8080
export TSH_WORKER_PORT=5555

# Then run
./start.sh
```

## Troubleshooting

### PostgreSQL connection errors

```bash
# Check if PostgreSQL is running
pg_isready -h 127.0.0.1 -p 5432

# Check user exists
psql -U postgres -c "SELECT * FROM pg_user WHERE usename='tsh'"

# Check database exists
psql -U postgres -c "SELECT datname FROM pg_database WHERE datname='tinyshell'"
```

### Spine server fails to start

Ensure these are set:
- `TSH_JOB_SIGNING_KEY` (≥32 bytes)
- `TSH_PG_DSN` (or use defaults)
- `TSH_GRPC_INSECURE_DEV=1` (for dev)

### gRPC connection errors

Make sure `TSH_GRPC_INSECURE_DEV=1` is set when testing locally.

## Development Workflow

1. Edit source files in `server/`, `client/`, `spine/`, etc.
2. Modify CMakeLists.txt if adding new files
3. Run `./start.sh` - it will rebuild automatically
4. Changes take effect after restart

## Clean Build

```bash
rm -rf build/
./start.sh
```
