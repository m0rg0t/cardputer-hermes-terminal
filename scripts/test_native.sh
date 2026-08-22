#!/bin/sh
set -eu

test_binary="${TMPDIR:-/tmp}/hermes-terminal-ws-frame-test"
stream_test_binary="${TMPDIR:-/tmp}/hermes-terminal-stream-text-test"
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/ws_frame_rules_test.cpp -o "$test_binary"
"$test_binary"
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Iinclude \
    test/stream_text_rules_test.cpp -o "$stream_test_binary"
"$stream_test_binary"
rm -f "$test_binary" "$stream_test_binary"
echo "Native protocol tests passed"
