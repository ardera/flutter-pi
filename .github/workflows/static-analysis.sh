#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")/../.."

mode="${1:-check}"
case "$mode" in
    check | update-baseline) ;;
    *)
        echo "usage: $0 [check|update-baseline]" >&2
        exit 1
        ;;
esac

clang_version="${CLANG_VERSION:-23}"
jobs="${JOBS:-$(nproc)}"
out="build/static-analysis"

export CC_ANALYZER_BIN="clangsa:$(command -v "clang-$clang_version");clang-tidy:$(command -v "clang-tidy-$clang_version");gcc:$(command -v gcc)"

allow_reports() {
    "$@" || [ $? -eq 2 ]
}

configure() {
    local name="$1"
    shift

    cmake -S . -B "$out/$name" -GNinja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$@"
}

minimal=(
    -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=OFF
    -DBUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=OFF
    -DENABLE_SESSION_SWITCHING=OFF
    -DBUILD_TEXT_INPUT_PLUGIN=OFF
    -DBUILD_RAW_KEYBOARD_PLUGIN=OFF
)

configure full \
    -DENABLE_OPENGL=ON \
    -DENABLE_VULKAN=ON \
    -DBUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN=ON \
    -DBUILD_GSTREAMER_AUDIO_PLAYER_PLUGIN=ON \
    -DENABLE_SESSION_SWITCHING=ON \
    -DBUILD_CHARSET_CONVERTER_PLUGIN=ON \
    -DENABLE_TESTS=ON

configure opengl-only "${minimal[@]}" \
    -DENABLE_OPENGL=ON \
    -DENABLE_VULKAN=OFF

configure vulkan-only "${minimal[@]}" \
    -DENABLE_OPENGL=OFF \
    -DTRY_ENABLE_OPENGL=OFF \
    -DENABLE_VULKAN=ON

rm -rf "$out/reports" "$out/html"

reports=()
for config in full opengl-only vulkan-only; do
    CodeChecker analyze "$out/$config/compile_commands.json" \
        --config .codechecker.json \
        --jobs "$jobs" \
        --output "$out/reports/$config"
    reports+=("$out/reports/$config")
done

allow_reports CodeChecker parse "${reports[@]}" \
    --config .codechecker.json \
    --export html \
    --output "$out/html" > /dev/null

allow_reports CodeChecker parse "${reports[@]}" \
    --config .codechecker.json \
    --trim-path-prefix "$PWD/" \
    --export sarif \
    --output "$out/results.raw.sarif" > /dev/null

jq 'walk(if type == "object" and has("uri") then .uri |= sub("^file://"; "") else . end)
    | .runs[].results |= unique_by([
    .ruleId,
    .message.text,
    .locations[0].physicalLocation.artifactLocation.uri,
    .locations[0].physicalLocation.region.startLine,
    .locations[0].physicalLocation.region.startColumn
])' "$out/results.raw.sarif" > "$out/results.sarif"

if [ "$mode" = update-baseline ]; then
    allow_reports CodeChecker parse "${reports[@]}" \
        --config .codechecker.json \
        --export baseline \
        --output .codechecker.baseline > /dev/null
    exit 0
fi

CodeChecker cmd diff \
    --basename .codechecker.baseline \
    --newname "${reports[@]}" \
    --new
