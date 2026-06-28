#!/usr/bin/env bash
#
# Fetch the fonts keyboard_test bundles for text + IME/emoji rendering.
#
# The fonts are NOT committed to the repository (see .gitignore); run this
# script once before `emb bundle` to populate ./fonts/. All are unmodified
# upstream releases, pinned to a version and verified by SHA-256.
#
#   DejaVu Sans / DejaVu Sans Mono   Bitstream Vera + Public Domain
#       https://github.com/dejavu-fonts/dejavu-fonts  (release version_2_37)
#   Noto Color Emoji (COLRv1)        SIL Open Font License 1.1
#       https://github.com/googlefonts/noto-emoji  (tag v2.047)
#
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dst="$here/fonts"
mkdir -p "$dst"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

verify() { # file sha256
  printf '%s  %s\n' "$2" "$1" | sha256sum -c - >/dev/null
}

# DejaVu Sans (text) + DejaVu Sans Mono (key-event log) ship in one tarball.
dejavu_url="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip"
curl -fsSL -o "$tmp/dejavu.zip" "$dejavu_url"
verify "$tmp/dejavu.zip" "7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a"
unzip -o -j "$tmp/dejavu.zip" \
  "dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf" \
  "dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono.ttf" -d "$dst" >/dev/null
verify "$dst/DejaVuSans.ttf"     "7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954"
verify "$dst/DejaVuSansMono.ttf" "b4a6c3e4faab8773f4ff761d56451646409f29abedd68f05d38c2df667d3c582"

# Noto Color Emoji (COLRv1).
noto_url="https://github.com/googlefonts/noto-emoji/raw/v2.047/fonts/Noto-COLRv1.ttf"
curl -fsSL -o "$dst/NotoColorEmoji.ttf" "$noto_url"
verify "$dst/NotoColorEmoji.ttf" "23549f29b5ad741fcb4c025b8dc44652ff0f459892467ebcccec1e6bbe839b44"

echo "Fonts fetched into $dst"
