#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cargo build --release --target wasm32-unknown-unknown
install -m 0644 "${CARGO_TARGET_DIR:-target}/wasm32-unknown-unknown/release/senkos_splash_web.wasm" ../../site/splash.wasm
