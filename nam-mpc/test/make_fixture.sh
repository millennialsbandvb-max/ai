#!/bin/sh
# Test folder for the offline test: make_fixture.sh <dir> <NeuralAmpModelerCore example_models dir>
# <dir>/Models: a few of NAM's example models (untrained test weights: they prove loading and processing, not tone).
# <dir>/IRs: a synthetic 48 kHz cab IR, longer than the plugin keeps, so resampling and truncation both run.
set -e
D="$1"; EX="$2"
rm -rf "$D" && mkdir -p "$D/Models/sub" "$D/IRs"
cp "$EX/wavenet.nam" "$EX/lstm.nam" "$D/Models/"
cp "$EX/A2.nam" "$D/Models/sub/"   # a slimmable model, one folder down
cp "$EX/wavenet_a1_standard.nam" "$D/Models/"
# macOS "._" twins and hidden folders, as a Mac leaves on a FAT/exFAT card: must be ignored
printf '\0\5\26\7Mac OS X        ' > "$D/Models/._A2.nam"
printf 'junk' > "$D/Models/._wavenet.nam"
mkdir -p "$D/Models/.Trashes" && cp "$EX/lstm.nam" "$D/Models/.Trashes/hidden.nam"
python3 - "$D/IRs/Test Cab.wav" <<'PY'
import math, random, struct, sys, wave
random.seed(1)
n = 2400
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<h", int(30000 * (1 if i == 0 else random.uniform(-1, 1) * math.exp(-i / 300)))) for i in range(n)))
w.close()
PY
echo "fixture: $D"
