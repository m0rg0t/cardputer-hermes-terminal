#!/bin/sh
# Portable rule tests: every test/*.cpp is a standalone host program that
# includes only header-only rules from include/hermes_terminal.
set -eu

cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hermes-terminal.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
for source in test/*_test.cpp; do
    binary="$tmp/$(basename "$source" .cpp)"
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
        "$source" -o "$binary"
    "$binary"
    rm -f "$binary"
done
echo "Native protocol tests passed"

