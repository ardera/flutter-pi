# Reproducing the OpenGL video benchmarks

This guide uses the external [DSlideShow benchmark application](https://github.com/DisDis/dslideshow/tree/master/benchmarks)
to exercise photo/video transitions, two simultaneous video players, seeks,
looping and repeated player disposal. It complements the
[native regression tests](video-player-lifecycle.md#native-regression-tests).
The harness and its instrumentation live in DSlideShow, outside flutter-pi.

## Prepare one application package

Use a prebuilt `benchmarks-pi5-release-*.tar.gz` package supplied with the
reproduction, or build one on a Linux development machine. A package contains
`bundle/` (AOT code, matching Flutter engine and assets), `run.sh`, `config/`
and `tool/`. Build it once and use that exact package for both native binaries.

For a source build, prepare a [DSlideShow checkout](https://github.com/DisDis/dslideshow)
with the sibling dependencies described in its
[build README](https://github.com/DisDis/dslideshow/blob/master/dslideshow_flutter/README.md#локальные-зависимости).
In particular, `backend/pubspec.yaml` expects these paths relative to `backend/`:

* `../../googleapis/discoveryapis_generator/googleapis` (a locally generated
  client including Photos Library, not a plain clone of googleapis);
* `../../flutter_packages/packages/flutter_gpiod`;
* `../../flutter_packages/packages/dpms`.

These are Dart build dependencies; the benchmark does not require the native
DPMS patch. If the generated client is unavailable, use the prebuilt package.
The harness requires Flutter >= 3.47 / Dart >= 3.13 and a `flutterpi_tool` version
supporting `--cpu=pi5`; the recorded reproduction used Flutter 3.47.7 / Dart 3.13.5.
For the first source build, generate the fixtures on the development machine
using Python with Pillow and ffmpeg with libx264, then build the package:

```sh
cd benchmarks
python3 tool/generate_media.py
flutter pub get
flutter analyze --no-pub
flutter test --no-pub
python3 -m unittest discover -s tool -p 'test_*.py'
bash -n run.sh build_aot_pi5.sh
./build_aot_pi5.sh
```

The script prints the package path under `build/` and leaves an expanded copy
under `build/package/`. Record the DSlideShow commit, any local changes, SDK and
tool versions, and the package SHA256. The package's `build-info.json` also
records build provenance. Prebuilt packages already include
large JPEGs and two moving 1920x1080 H.264/30 fps clips, each eight seconds long. Pillow/ffmpeg are not needed on the Pi.

Copy the expanded package to a dedicated directory on the Pi, while no benchmark
is running. Replace the host, directory and package suffix in this example:

```sh
rsync -a --checksum --partial --timeout=600 \
  -e 'ssh -o BatchMode=yes -o ConnectTimeout=120 -o ServerAliveInterval=30 -o ServerAliveCountMax=10' \
  build/package/benchmarks-pi5-release-XXXXXXXX/ \
  pi@raspberrypi:/home/pi/video-benchmarks/
ssh -o BatchMode=yes -o ConnectTimeout=120 \
  -o ServerAliveInterval=30 -o ServerAliveCountMax=10 pi@raspberrypi
```

For a prebuilt package, copy it to the Pi and extract it there instead:

```sh
mkdir -p /home/pi/video-benchmarks
tar -xzf /path/to/benchmarks-pi5-release-XXXXXXXX.tar.gz \
  -C /home/pi/video-benchmarks
```

Keep the same `bundle/`, including its engine, throughout the comparison.

## Prepare the native baseline and candidate

On the Pi, use separate source/build directories and the same compiler, flags
and existing libraries for both revisions. The flutter-pi
[development dependencies](../README.md#dependencies) must already be available.
Choose `BASE_REF` as the master commit immediately before the fix, and
`FIX_REF` as the branch/commit containing it; both must exist in the checkout.
From a flutter-pi Git checkout, for example:

```sh
BASE_REF=8b86c52a004ec2ca7f98a65e5079219679002c61
FIX_REF=codex/video-player-lifecycle-upstream
git worktree add --detach ../flutter-pi-video-baseline "$BASE_REF"
git worktree add --detach ../flutter-pi-video-candidate "$FIX_REF"
for variant in baseline candidate; do
  src="../flutter-pi-video-$variant"
  cmake -S "$src" -B "$src/build-video" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE='-O3 -DNDEBUG -g' \
    -DENABLE_OPENGL=ON -DTRY_ENABLE_OPENGL=OFF -DENABLE_VULKAN=OFF \
    -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=ON \
    -DTRY_BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF -DLTO=OFF
  cmake --build "$src/build-video" -j3
  git -C "$src" rev-parse HEAD
  sha256sum "$src/build-video/flutter-pi"
done
```

Pass these executables to the harness by absolute path; installation is
unnecessary. Preserve the installed production runtime. Hold the OS, firmware,
Mesa/GStreamer, display mode, media, SDK and engine fixed between runs.
The original driver-crash reproduction used Pi 5, Mesa 24.2.8 and GStreamer 1.22.
The application engine must be compatible with both embedders.

## Select short video scenarios

The remaining commands run **on the Pi**, in the deployed package directory.
The examples target the original 2560x1600 display. For a different display,
set `BENCHMARK_VIDEO_MODE` and both `expected_width` / `expected_height` in each
JSON to that display's native mode, identically for baseline and candidate.

```sh
cd /home/pi/video-benchmarks
mkdir -p config
cat > config/video-transitions.json <<'JSON'
{
  "suite": "smoke",
  "effects": ["Baseline", "Cube", "Fade", "Blinds"],
  "media_cases": ["photo-video", "video-photo", "video-video"],
  "grids": [100],
  "backends": ["canvas"],
  "warmups": 1,
  "repeats": 3,
  "transition_ms": 1000,
  "capture_frames": false
}
JSON
cat > config/video-loop.json <<'JSON'
{
  "suite": "smoke",
  "effects": ["Baseline"],
  "media_cases": ["video-video"],
  "grids": [100],
  "backends": ["canvas"],
  "warmups": 0,
  "repeats": 3,
  "transition_ms": 20000,
  "transition_timeout_seconds": 120,
  "capture_frames": false
}
JSON
sha256sum bundle/app.so config/video-*.json
find bundle -type f -name '*flutter_engine*' -exec sha256sum {} \;
```

The first configuration covers 12 cases / 36 measured transitions. The second
keeps two players active beyond two fixture durations to test repeated EOS and
flushing seeks. `FixedAnimationController` intentionally slows transitions under
load: inspect `actual_ms`, not just `transition_ms`, when judging loop coverage.
For broader composition coverage, repeat with explicit effect filters including
`Mosaic` / `MosaicFade` and grids `[100, 150, 200]`; preserve media resolution.

## Run independently of SSH and restore the normal application

The display must be free: only one embedder can own DRM. `run.sh` itself does
not stop services. The supervisor below assumes the normal application is the
initially running `dslideshow` service, and that `sudo -n service dslideshow
stop/start` is authorized without an interactive password. Adapt that service
name for another installation. Run every benchmark through the same supervisor.

Create this script in the deployed package directory:

```sh
cat > run-managed.sh <<'BASH'
#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
job=${1:?Usage: run-managed.sh UNIQUE_JOB [run.sh arguments]}
shift
[[ "$job" =~ ^[a-zA-Z0-9_-]+$ ]] || exit 2
control="$PWD/managed/$job"
mkdir -p "$control"
child=''
restore_needed=0
stopping=0
cleanup() {
  local result=$?
  trap - EXIT
  trap '' INT TERM
  if [[ -n "$child" ]]; then
    kill -TERM "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
  fi
  if ((restore_needed)); then
    if ! sudo -n service dslideshow start; then
      echo 'ERROR: could not restore dslideshow' >&2
      ((result != 0)) || result=1
    fi
  fi
  printf '%s\n' "$result" > "$control/exit-code"
  rm -f "$control/pid"
  exit "$result"
}
stop() {
  stopping=1
  if [[ -n "$child" ]]; then kill -TERM "$child" 2>/dev/null || true; fi
}
# Separate from the lock held by run.sh; covers service restoration too.
exec 8>"$PWD/managed-supervisor.lock"
flock -n 8 || { echo 'Another managed run is active' >&2; exit 2; }
[[ ! -e "$control/started-utc" ]] || { echo 'Use a new job name' >&2; exit 2; }
trap cleanup EXIT
trap stop INT TERM
printf '%s\n' "$$" > "$control/pid"
printf '%q ' "$@" > "$control/arguments"
date -u +%Y-%m-%dT%H:%M:%SZ > "$control/started-utc"
((stopping == 0)) || exit 130
restore_needed=1
sudo -n service dslideshow stop
((stopping == 0)) || exit 130
./run.sh "$@" &
child=$!
if ((stopping)); then kill -TERM "$child" 2>/dev/null || true; fi
code=0
wait "$child" || code=$?
if ((stopping)); then wait "$child" 2>/dev/null || true; code=130; fi
child=''
printf '%s\n' "$code" > "$control/benchmark-exit-code"
exit "$code"
BASH
chmod +x run-managed.sh
bash -n run-managed.sh
```

Select one runtime and scenario, then launch a unique job. These paths are
examples; replace `RUNTIME` with the absolute build path from the previous step:

```sh
RUNTIME=/home/pi/flutter-pi-video-baseline/build-video/flutter-pi
LABEL=baseline
SCENARIO=video-transitions
job="$(date -u +%Y%m%dT%H%M%SZ)-$LABEL-$SCENARIO"
mkdir -p "managed/$job"
nohup ./run-managed.sh "$job" --flutter-pi "$RUNTIME" --bundle "$PWD/bundle" \
  --config "$PWD/config/$SCENARIO.json" --renderers opengl --no-vulkan-video \
  --timeout 1200 --output "$PWD/reports/$job" \
  > "managed/$job/launcher.log" 2>&1 < /dev/null &
printf '%s\n' "$job" > managed/latest-job
```

You may disconnect SSH. After reconnecting, inspect the job:

```sh
cd /home/pi/video-benchmarks
job=$(cat managed/latest-job)
tail -n 60 "managed/$job/launcher.log"
test ! -f "managed/$job/exit-code" || cat "managed/$job/exit-code"
systemctl is-active dslideshow || true
```

Wait for `managed/JOB/exit-code`, which is written after service restoration.
Then repeat with `SCENARIO=video-loop`, and repeat both configurations with
`LABEL=candidate` and the candidate executable. Compare only matching scenarios.
To stop a running job, send `kill -TERM "$(cat "managed/$job/pid")"` and wait
for cleanup. The trap cannot recover from a power loss or a SIGKILL of the
supervisor: first ensure the test embedder has exited, then restore the service.

## Inspect and share the results

`BENCHMARK_ARCHIVE=...tar.gz` in `launcher.log` identifies each complete result
archive. Copy the **whole archive**, including failed attempts, back to the
development machine, for example:

```sh
rsync -a --partial --append-verify --timeout=600 \
  -e 'ssh -o BatchMode=yes -o ConnectTimeout=120 -o ServerAliveInterval=30 -o ServerAliveCountMax=10' \
  pi@raspberrypi:/home/pi/video-benchmarks/reports/JOB/ARCHIVE.tar.gz ./
```

Inspect `report.html` / `summary.json` and each `opengl/attempt-NNN/` directory:

* `native.log`, `errors.jsonl`, `launcher-status.json` and the summary's active
  operation / crash count must remain available. The launcher may resume remaining
  cases after a crash; that does not turn the crashed run into a pass.
* `runs.jsonl` contains `actual_ms`, FrameTiming statistics, preparation and a
  `videos` array. In **every measured loop row**, check both players, their
  `position_advances`, `position_ms`, `is_playing` and `error`, against the clip's
  decoder-reported `preparation.media[].duration_ms`. Require an interval longer
  than two clip durations and sustained progress from both players.
* The position check in a raw `passed` row only requires at least one advance. It is
  insufficient for loop acceptance. In the observed failure, one eight-second
  clip ended at `position_ms=8000` with about 81 advances during a 20-second
  window, while its peer continued with about 199 advances. Treat this as a
  stalled loop even if the row says `passed`; missing measurements also fail.
* Compare UI/raster frame tails, preparation, RSS/HWM, fd/thread trends and
  cleanup errors under identical conditions. Keep missing values as missing.
  A short run is not proof of absence of leaks or driver races.
* Inspect `video-probes/` and `metadata.video_texture_probes` for changing video
  pixels, and observe the physical display for stalls, black frames or corruption.
  Position counters and offscreen captures alone do not verify physical scanout.

Include source/binary/package/config hashes, display mode, library versions and
all raw logs with the MR. If a native crash occurs, keep its active operation and
collect a backtrace in a separate diagnostic run; debugger timing is unsuitable
for performance comparisons. These bounded checks use OpenGL; no Vulkan or
24-hour run is required for this reproduction.
