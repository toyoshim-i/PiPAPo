# PiPAPo — gh-pages branch

This orphan branch is published via GitHub Pages. It hosts a browser build of
the PPAP PC/XT port running under [v86](https://github.com/copy/v86).

## Contents
- `index.html` — v86 boot page
- `v86/` — pinned v86 runtime + SeaBIOS (see `LICENSE-v86`)
- `ppap_pcxt_hdd.img` — prebuilt 64 MB HDD image, loaded by v86 via async HTTP Range requests

## Updating the HDD image
From a checkout of `main`:

```sh
./scripts/run.sh --build pcxt
git worktree add /tmp/ppap-pages gh-pages
cp build/pcxt/ppap_pcxt_hdd.img /tmp/ppap-pages/
cd /tmp/ppap-pages
git commit -am "refresh hdd image"
git push origin gh-pages
git worktree remove /tmp/ppap-pages
```

## Updating the v86 runtime
Re-download from the upstream release and commit the bundle:

```sh
cd /tmp/ppap-pages/v86
for f in libv86.js v86.wasm; do
  curl -sL -o "$f" "https://github.com/copy/v86/releases/download/latest/$f"
done
for f in seabios.bin vgabios.bin; do
  curl -sL -o "$f" "https://github.com/copy/v86/raw/master/bios/$f"
done
```

## Updating xterm.js (serial console renderer)
```sh
cd /tmp/ppap-pages/v86
VER=5.5.0
curl -sL -o xterm.js  "https://cdn.jsdelivr.net/npm/@xterm/xterm@${VER}/lib/xterm.js"
curl -sL -o xterm.css "https://cdn.jsdelivr.net/npm/@xterm/xterm@${VER}/css/xterm.css"
```
