#!/bin/sh
# Portable rule tests: every test/*.cpp is a standalone host program that
# includes only header-only rules from include/hermes_terminal.
set -eu

cd "$(dirname "$0")/.."
tmp="${TMPDIR:-/tmp}"
for source in test/*_test.cpp; do
    binary="$tmp/hermes-terminal-$(basename "$source" .cpp)"
    c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
        "$source" -o "$binary"
    "$binary"
    rm -f "$binary"
done
echo "Native protocol tests passed"
