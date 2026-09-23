#!/usr/bin/env sh
# Runs every native test: the C++ firmware-logic suite and the Node.js report-page suite.
# Needs a C++17 compiler and Node.js 18 or newer. No Arduino toolchain is involved.
set -eu
cd "$(dirname "$0")/.."

build_dir="tests/build"
mkdir -p "$build_dir"

echo "== firmware logic (C++)"
c++ -std=c++17 -Wall -Wextra -Werror tests/firmware_logic_test.cpp -o "$build_dir/firmware_logic_test"
"$build_dir/firmware_logic_test"

echo "== report page (Node.js)"
node tests/report_html_test.mjs

echo "== all native tests passed"
