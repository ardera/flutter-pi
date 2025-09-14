#!/bin/sh

ENGINE_VERSION="$(curl -s https://raw.githubusercontent.com/flutter/flutter/stable/bin/internal/engine.version)"

if [ ! -f "third_party/flutter_embedder_header/include/flutter_embedder.h" ]; then
    echo "Incorrect working directory. Please launch this script with the flutter-pi repo root as the working directory, using 'tools/roll_embedder_header.sh'.".
    exit 1
fi

curl -o third_party/flutter_embedder_header/include/flutter_embedder.h "https://raw.githubusercontent.com/flutter/engine/$ENGINE_VERSION/shell/platform/embedder/embedder.h"
echo "$ENGINE_VERSION" > third_party/flutter_embedder_header/engine.version
