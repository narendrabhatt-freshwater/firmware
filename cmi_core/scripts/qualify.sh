                                                                                                                                                                                                                                                                                                                                                                                                              #!/usr/bin/env bash
# Copyright 2026 Epic Games, Inc. All Rights Reserved.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${CMI_QUALIFY_BUILD:-$ROOT/build/qualification}"
INSTALL="${CMI_QUALIFY_INSTALL:-$ROOT/build/qualification-install}"
CONSUMER="${CMI_QUALIFY_CONSUMER:-$ROOT/build/qualification-consumer}"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCMI_CORE_BUILD_CLI=ON
cmake --build "$BUILD" --parallel
ctest --test-dir "$BUILD" --output-on-failure
cmake --install "$BUILD" --prefix "$INSTALL"
cmake -S "$ROOT/examples/sdk" -B "$CONSUMER" \
  -DCMAKE_BUILD_TYPE=Release -DCMI_CORE_EXAMPLE_USE_INSTALLED=ON \
  -DCMAKE_PREFIX_PATH="$INSTALL"
cmake --build "$CONSUMER" --parallel
"$BUILD/cmi-play" --help >/dev/null
"$CONSUMER/cmi_core_example" --help >/dev/null

echo "cmi_core software qualification passed"
echo "Hardware qualification remains a separate requirement in RELEASE.md"
