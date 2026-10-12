#!/bin/sh
# Run the desktop build headless against a test script and convert captures.
# usage: TESTDIR=/path/to/testport tools/runtest.sh script.txt [--set key=value ...]
# TESTDIR must contain gamedata/ (the DOS game), dosbox_pure_libretro.so and shots/.
# Script commands: wait N | tap X Y [right] | touchdown/touchmove X Y | touchup |
#   action NAME | press/release NAME | click GX GY | move GX GY | key K |
#   shot FILE | frame FILE | info | quit
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=${TESTDIR:?set TESTDIR}
cp "$ROOT/build/usds-x86_64" "$T/usds"
SCRIPT=$1; shift
cd "$T"
SDL_AUDIODRIVER=dummy timeout 600 xvfb-run -a -s "-screen 0 1280x1600x24" ./usds --nothrottle --script "$SCRIPT" \
  --set window_mode=single --set window_height=1536 --set window_width=1024 "$@" 2>&1 | grep -E "info:|error|Error|touch:|script: unknown" || true
python3 - "$T/shots" <<'PY'
import glob, os, sys
from PIL import Image
os.chdir(sys.argv[1])
for f in glob.glob('*.bmp'):
    im = Image.open(f).convert('RGB')
    if im.size == (320, 200): im = im.resize((960, 600), Image.NEAREST)
    im.save(f[:-4] + '.png'); os.remove(f)
PY
