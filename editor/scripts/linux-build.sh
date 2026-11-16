#!/usr/bin/env bash
# Builds the Linux packages (Forge Studio, Forge Player, Forge Server) on Linux
# or inside WSL. Called by `npm run dist:linux` on Windows, or directly on Linux:
#   bash scripts/linux-build.sh <project-dir> [studio] [player] [server]
# Node.js and Rust are installed into the user's home directory if missing
# (no sudo needed). The project is copied to ~/forge-build so the Linux
# node_modules do not clash with the Windows ones; results are copied back
# into <project-dir>/release/.
set -euo pipefail

SRC="$(cd "$1" && pwd)"; shift
TARGETS="${*:-studio player server}"
WORK="$HOME/forge-build"
NODE_VERSION="22.20.0"
NODE_DIR="$HOME/.local/forge-node"

# --- toolchains -------------------------------------------------------------
if ! command -v node >/dev/null 2>&1 || [[ "$(command -v node)" == /mnt/* ]]; then
  if [ ! -x "$NODE_DIR/bin/node" ]; then
    echo "[linux] installing Node.js $NODE_VERSION to $NODE_DIR"
    mkdir -p "$NODE_DIR"
    curl -fsSL "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz" | tar -xJ -C "$NODE_DIR" --strip-components=1
  fi
  export PATH="$NODE_DIR/bin:$PATH"
fi
if [ ! -x "$HOME/.cargo/bin/cargo" ] && ! command -v cargo >/dev/null 2>&1; then
  echo "[linux] installing Rust (rustup) to ~/.cargo"
  curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal
fi
export PATH="$HOME/.cargo/bin:$PATH"
rustup target list --installed | grep -q wasm32-unknown-unknown || rustup target add wasm32-unknown-unknown
command -v cc >/dev/null 2>&1 || { echo "[linux] a C compiler is needed for Rust: sudo apt install build-essential"; exit 1; }
echo "[linux] node $(node --version), $(cargo --version)"

# --- build in a Linux-native copy ------------------------------------------
mkdir -p "$WORK"
rsync -a --delete \
  --exclude node_modules --exclude dist --exclude release \
  --exclude 'engine/target' --exclude 'server/target' --exclude 'server-data' \
  "$SRC/" "$WORK/"
cd "$WORK"
if [ -f package-lock.json ]; then npm ci --no-audit --no-fund; else npm install --no-audit --no-fund; fi
node scripts/dist.mjs $TARGETS

# --- copy results back -------------------------------------------------------
for t in studio player server; do
  [ -d "release/$t" ] || continue
  mkdir -p "$SRC/release/$t"
  find "release/$t" -maxdepth 1 -type f \( -name '*.AppImage' -o -name '*.tar.gz' \) -exec cp -f {} "$SRC/release/$t/" \;
  [ "$t" = server ] && cp -rf release/server/ForgeServer-*-linux-x64 "$SRC/release/server/" || true
done
echo "[linux] done – Linux packages copied to $SRC/release/"
