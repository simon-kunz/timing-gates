#!/bin/sh
# Build the timing component for the host and run its unit tests.
set -e
here=$(cd "$(dirname "$0")" && pwd)
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$here/build" -j
ctest --test-dir "$here/build" --output-on-failure
