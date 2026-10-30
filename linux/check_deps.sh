#!/bin/bash
# Linux build: the newest glibc symbol version and the shared libraries each binary needs (what SteamOS must have)
source "$(dirname "$(readlink -f "$0")")/env.sh"
for f in "$RUNTIME" "$CODE_SO"; do
  echo "== $f"
  [ -f "$f" ] || { echo "   (not built)"; continue; }
  objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -2
  readelf -d "$f" | grep NEEDED
done
