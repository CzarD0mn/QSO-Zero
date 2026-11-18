#!/usr/bin/env bash
# Called by AppBuilder's pack-deb.sh from the project root.
set -euo pipefail
make -j"$(nproc)" VERSION="${VERSION:-0.1.1}"
