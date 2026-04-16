#!/usr/bin/env bash
# Download vendored dependencies (mongoose, cJSON, stb_image) into vendor/.
#
# Run once after cloning:   ./scripts/fetch-vendor.sh
#
# Versions are pinned to specific git tags so the build is reproducible.

set -euo pipefail

MONGOOSE_VERSION="7.14"
CJSON_VERSION="v1.7.18"
STB_IMAGE_COMMIT="5c205738c191bcb0abc65c4febfa9bd25ff35234"   # 2024-06

VENDOR_DIR="$(cd "$(dirname "$0")/.." && pwd)/vendor"
mkdir -p "$VENDOR_DIR"

have() { command -v "$1" >/dev/null 2>&1; }
if ! have curl; then echo "error: curl required" >&2; exit 1; fi

echo "==> Vendor dir: $VENDOR_DIR"

fetch() {
  local url="$1" dest="$2"
  if [[ -f "$dest" ]]; then
    echo "    exists   $dest"
    return
  fi
  echo "    download $dest"
  curl -fsSL "$url" -o "$dest"
}

# Mongoose
fetch "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.c" \
      "$VENDOR_DIR/mongoose.c"
fetch "https://raw.githubusercontent.com/cesanta/mongoose/${MONGOOSE_VERSION}/mongoose.h" \
      "$VENDOR_DIR/mongoose.h"

# cJSON
fetch "https://raw.githubusercontent.com/DaveGamble/cJSON/${CJSON_VERSION}/cJSON.c" \
      "$VENDOR_DIR/cJSON.c"
fetch "https://raw.githubusercontent.com/DaveGamble/cJSON/${CJSON_VERSION}/cJSON.h" \
      "$VENDOR_DIR/cJSON.h"

# stb_image
fetch "https://raw.githubusercontent.com/nothings/stb/${STB_IMAGE_COMMIT}/stb_image.h" \
      "$VENDOR_DIR/stb_image.h"

echo "==> Done."
echo
echo "Versions:"
echo "    mongoose   = $MONGOOSE_VERSION"
echo "    cJSON      = $CJSON_VERSION"
echo "    stb_image  = $STB_IMAGE_COMMIT"
