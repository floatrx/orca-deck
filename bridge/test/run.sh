#!/bin/sh
# Runs bridge/test/core_test.lua inside Hammerspoon's Lua 5.4.
# The tests load bridge/core.lua only: no server starts and no key is pressed.
DIR=$(cd "$(dirname "$0")" && pwd)
out=$(hs -c "return dofile('$DIR/core_test.lua')" </dev/null 2>&1)
echo "$out"
case "$out" in
  *" passed, 0 failed"*) exit 0 ;;
  *) exit 1 ;;
esac
