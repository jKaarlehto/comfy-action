#!/usr/bin/env bash
# Local pre-push gate: compile the transport-agnostic conformance core and the
# C++ client compile-check against the checked-out interface with plain g++, then
# run the stub self-test. This catches interface/mock API mismatches (the class
# of break that fails the Docker build) without cmake or IXWebSocket. The
# IXWebSocket transport adapters build only in the Docker runner and are NOT
# covered here.
#
# Usage: local_build_check.sh [path-to/cpp/notch_comfy_client]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mock_root="$(cd "$here/.." && pwd)"
action_root="$(cd "$mock_root/.." && pwd)"

iface="${1:-$action_root/../ComfyUI/custom_nodes/ComfyUI-Notch/cpp/notch_comfy_client}"
if [ ! -f "$iface/CMakeLists.txt" ]; then
  echo "interface source not found at: $iface" >&2
  echo "pass the path to cpp/notch_comfy_client as the first argument" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

common=(-std=c++14 -Wall -Wextra
  -I "$iface/include" -I "$iface/third_party/jsonxx" -I "$mock_root/src")

echo "== compile_check (client interface) =="
g++ "${common[@]}" \
  "$iface/tests/compile_check.cpp" "$iface/src/client_interface.cpp" \
  "$iface/third_party/jsonxx/jsonxx.cc" -o "$tmp/compile_check"
"$tmp/compile_check"
echo "compile_check OK"

echo "== conformance stub self-test =="
g++ "${common[@]}" \
  "$mock_root/tests/stub_check.cpp" "$mock_root/src/matrix.cpp" "$mock_root/src/case_logger.cpp" \
  "$mock_root/src/hash_utils.cpp" "$mock_root/src/delivery_types.cpp" \
  "$iface/src/client_interface.cpp" "$iface/third_party/jsonxx/jsonxx.cc" -o "$tmp/stub_check"
(cd "$tmp" && "$tmp/stub_check")
rm -rf "$tmp/stub-out"

echo "== verifier self-test =="
g++ "${common[@]}" \
  "$mock_root/tests/verify_check.cpp" \
  "$mock_root/src/verify/verify_byte_exact.cpp" "$mock_root/src/verify/verify_integrity.cpp" \
  "$mock_root/src/verify/verify_structural.cpp" \
  "$mock_root/src/case_logger.cpp" "$mock_root/src/hash_utils.cpp" \
  "$iface/src/client_interface.cpp" "$iface/third_party/jsonxx/jsonxx.cc" -o "$tmp/verify_check"
"$tmp/verify_check"

echo "local build check OK"
