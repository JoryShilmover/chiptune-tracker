#!/bin/sh
# Builds and runs the unit tests.
set -e
cd "$(dirname "$0")/.."
swift run -c release spu-tests
