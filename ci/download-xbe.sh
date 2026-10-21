#!/usr/bin/env bash
# Downloads the user's own game into assets/ for the recompiler. The URL is a
# repository secret: a direct link (private release asset, object storage, ...),
# never a page that needs a login. It may be the default.xbe itself or a disc
# image (.iso); the XBE is extracted from an image with tools/xdvdfs.py. The
# file is never committed.
set -euo pipefail

out="${1:-assets/default.xbe}"

: "${FORZA_XBE_URL:?set FORZA_XBE_URL to a direct download URL for your own default.xbe or ISO}"

mkdir -p "$(dirname "$out")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
download="$work/download.bin"

# --silent keeps the URL (and its signature) out of the log; --show-error still
# prints the reason on failure. Files in a private GitHub repository need a
# token, which is only ever sent to GitHub hosts, never to arbitrary URLs.
curl_args=(--fail --location --retry 3 --retry-delay 5 --silent --show-error)
case "$FORZA_XBE_URL" in
    https://raw.githubusercontent.com/*|https://api.github.com/*|https://github.com/*)
        if [ -n "${FORZA_XBE_TOKEN:-}" ]; then
            curl_args+=(-H "Authorization: Bearer $FORZA_XBE_TOKEN")
            case "$FORZA_XBE_URL" in
                https://api.github.com/*) curl_args+=(-H "Accept: application/vnd.github.raw") ;;
            esac
        fi
        ;;
esac
curl "${curl_args[@]}" "$FORZA_XBE_URL" -o "$download"

if [ -n "${FORZA_XBE_SHA256:-}" ]; then
    if ! printf '%s  %s\n' "$FORZA_XBE_SHA256" "$download" | sha256sum --check --status; then
        echo "SHA-256 does not match FORZA_XBE_SHA256; refusing the download" >&2
        exit 1
    fi
fi

if [ "$(head -c 4 "$download")" = "XBEH" ]; then
    mv "$download" "$out"
else
    # An Xbox disc image: only the executable is needed here; the phone mounts
    # the ISO itself when the player puts it in the Forza folder.
    python3 "$(dirname "${BASH_SOURCE[0]}")/../tools/xdvdfs.py" "$download" default.xbe "$out"
fi

size=$(stat -c %s "$out")
if [ "$size" -lt 1000000 ]; then
    echo "the extracted file is only $size bytes, that is not an Xbox executable" >&2
    rm -f "$out"
    exit 1
fi

printf 'wrote %s bytes to %s\n' "$size" "$out"
