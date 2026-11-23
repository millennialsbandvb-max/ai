#!/bin/sh
# Other Things: entry point for MPC OS 3.10.1+'s autorun.sh ScriptRunner (the MPC asks before it runs). Put this
# file, OtherThings/, VST2 Plugins/ (and HAKAI's Hakai/) at the top of the drive. With our 3.10.1 firmware image the
# add-on starts by itself at boot: then delete this file (it would only make the MPC ask again).
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec /bin/sh "$ROOT/OtherThings/start.sh"
