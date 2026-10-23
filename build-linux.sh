#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

test_build=false
smoke=false
package=false
for option in "$@"; do
  case "$option" in
    --test) test_build=true ;;
    --smoke) smoke=true ;;
    --package) package=true ;;
    *) echo "Usage: bash build-linux.sh [--test] [--smoke] [--package]" >&2; exit 2 ;;
  esac
done

# X11 also runs through XWayland on Wayland desktops and under WSLg.
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
cmake --build build-linux --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
if "$test_build"; then ctest --test-dir build-linux --output-on-failure; fi
if "$smoke"; then
  mkdir -p artifacts
  # Force the virtual X11 display when a WSLg/Wayland session is also present.
  env -u WAYLAND_DISPLAY LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a -s '-screen 0 1920x1200x24' \
    build-linux/coldedit --smoke-test --screenshot artifacts/linux-smoke.bmp --report artifacts/linux-smoke.json
fi
if "$package"; then
  cmake --install build-linux --prefix dist/ColdEdit-linux
  tar -czf dist/ColdEdit-linux-x64.tar.gz -C dist ColdEdit-linux
fi
