#!/bin/bash
# Feature 7: Binary Diversity Engine
# Injects a unique nonce into the project before every build to ensure unique binary signatures.

DIR="$(dirname "$0")/../common"
NONCE=$(head /dev/urandom | tr -dc A-Za-z0-9 | head -c 32 ; echo '')

echo "// Feature 7: Binary Diversity Nonce (DO NOT EDIT)" > "$DIR/version_nonce.h"
echo "#define TSH_BINARY_NONCE \"$NONCE\"" >> "$DIR/version_nonce.h"

echo "[Mutator] Injected new binary entropy: $NONCE"
