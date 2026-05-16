#!/usr/bin/env bash
set -euo pipefail

# BUG: operators copied development secrets from .env.example into production.
# FIX: generate fresh high-entropy secrets suitable for shell export.
printf 'export TSH_JOB_SIGNING_KEY="%s"\n' "$(openssl rand -hex 32)"
printf 'export TSH_API_TOKEN="%s"\n' "$(openssl rand -base64 32)"
