#!/usr/bin/env bash
# Setup PostgreSQL for TinyShell Spine Server
# This script creates the database and initializes the schema

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Configuration - match defaults in start.sh
DB_NAME="${TSH_DB_NAME:-tinyshell}"
DB_USER="${TSH_DB_USER:-tsh}"
DB_HOST="${TSH_DB_HOST:-127.0.0.1}"
DB_PORT="${TSH_DB_PORT:-5432}"

validate_identifier() {
    local name="$1"
    local value="$2"
    if [[ ! "$value" =~ ^[A-Za-z_][A-Za-z0-9_]{0,62}$ ]]; then
        echo "[TinyShell Setup] ERROR: $name must be a PostgreSQL identifier, got: $value" >&2
        exit 1
    fi
}

validate_identifier "TSH_DB_NAME" "$DB_NAME"
validate_identifier "TSH_DB_USER" "$DB_USER"

echo "[TinyShell Setup] Initializing PostgreSQL database..."
echo "  Database: $DB_NAME"
echo "  User: $DB_USER"
echo "  Host: $DB_HOST:$DB_PORT"

# Check if psql is available
if ! command -v psql &> /dev/null; then
    echo "[TinyShell Setup] ERROR: psql not found!"
    echo "Install PostgreSQL client with: sudo apt install postgresql-client"
    exit 1
fi

# Check if PostgreSQL server is running
if ! pg_isready -h "$DB_HOST" -p "$DB_PORT" &>/dev/null; then
    echo "[TinyShell Setup] ERROR: PostgreSQL server is not running at $DB_HOST:$DB_PORT"
    echo ""
    echo "Start PostgreSQL with one of these options:"
    echo ""
    echo "  Option 1: System PostgreSQL service"
    echo "    sudo systemctl start postgresql"
    echo ""
    echo "  Option 2: Docker container"
    echo "    docker run -d \\
      --name tsh-postgres \\
      -e POSTGRES_USER=tsh \\
      -e POSTGRES_PASSWORD= \\
      -e POSTGRES_DB=tinyshell \\
      -p 5432:5432 \\
      postgres:15"
    echo ""
    exit 1
fi

echo "[TinyShell Setup] PostgreSQL server is running ✓"

# Create superuser if running locally as root
if [[ "$DB_HOST" == "127.0.0.1" || "$DB_HOST" == "localhost" ]]; then
    # Try to create user as postgres superuser
    if sudo -u postgres psql -h "$DB_HOST" -v user="$DB_USER" -c "SELECT 1 FROM pg_user WHERE usename = :'user'" 2>/dev/null | grep -q 1; then
        echo "[TinyShell Setup] User $DB_USER already exists ✓"
    else
        echo "[TinyShell Setup] Creating PostgreSQL user: $DB_USER"
        sudo -u postgres psql -h "$DB_HOST" -c "CREATE USER \"$DB_USER\" WITH CREATEDB"
        echo "[TinyShell Setup] User created ✓"
    fi
    
    # Use postgres superuser for remaining operations
    DB_ADMIN_USER="postgres"
else
    # For remote databases, use the configured user
    DB_ADMIN_USER="$DB_USER"
fi

# Create database if it doesn't exist
if psql -h "$DB_HOST" -U "$DB_ADMIN_USER" -v db="$DB_NAME" -tc "SELECT 1 FROM pg_database WHERE datname = :'db'" 2>/dev/null | grep -q 1; then
    echo "[TinyShell Setup] Database $DB_NAME already exists ✓"
else
    echo "[TinyShell Setup] Creating database: $DB_NAME"
    psql -h "$DB_HOST" -U "$DB_ADMIN_USER" -c "CREATE DATABASE \"$DB_NAME\" OWNER \"$DB_USER\""
    echo "[TinyShell Setup] Database created ✓"
fi

# Initialize schema
echo "[TinyShell Setup] Initializing database schema..."
psql -h "$DB_HOST" -U "$DB_ADMIN_USER" -d "$DB_NAME" -f "$SCRIPT_DIR/db/001_execution_spine.sql"
echo "[TinyShell Setup] Schema initialized ✓"

echo ""
echo "[TinyShell Setup] PostgreSQL setup complete!"
echo ""
echo "To start TinyShell, run:"
echo "  ./start.sh"
echo ""
