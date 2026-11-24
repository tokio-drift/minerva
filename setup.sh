#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This setup script supports Ubuntu/Debian. Install a C++17 compiler, GNU Make 4.3+, Flex, Bison and SPIM manually on other systems." >&2
    exit 1
fi

APT=(apt-get)
if (( EUID != 0 )); then
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Run this script as root or install sudo first." >&2
        exit 1
    fi
    APT=(sudo apt-get)
fi

"${APT[@]}" update
"${APT[@]}" install -y build-essential flex bison spim
echo "Setup complete. Run make to build, then make test to run the demos."
