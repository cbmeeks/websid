#!/bin/sh
set -e
emcc -s VERBOSE=0 -Wno-pointer-sign -I./src -Os -O3  --memory-init-file 0 --closure 1 --llvm-lto 1 -s NO_FILESYSTEM=1 src/memory.c src/base.c src/cpu.c src/hacks.c src/cia.c src/vic.c src/rsidengine.c src/sid.c src/digi.c src/sidplayer.c -s EXPORTED_FUNCTIONS="['_alloc','_loadSidFile', '_playTune', '_getMusicInfo', '_getSampleRate', '_getSoundBuffer', '_getSoundBufferLen', '_computeAudioSamples', '_enableVoices', '_malloc', '_free']" -o htdocs/tinyrsid2.js -s 'EXTRA_EXPORTED_RUNTIME_METHODS=["ccall"]' -s WASM=0 -s SINGLE_FILE=1 -s BINARYEN_ASYNC_COMPILATION=0 -s 'BINARYEN_TRAP_MODE="clamp"'
cat shell-pre.js > htdocs/tinyrsid.js
cat htdocs/tinyrsid2.js >> htdocs/tinyrsid.js
cat shell-post.js >> htdocs/tinyrsid.js
cat htdocs/tinyrsid.js > htdocs/backend_tinyrsid.js
cat tinyrsid_adapter.js >> htdocs/backend_tinyrsid.js
