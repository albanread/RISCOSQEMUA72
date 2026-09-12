#!/bin/zsh
S="${0:A:h}"
N="${1:-shot}"
python3 - "$S/$N.ppm" <<PY
import os,sys; sys.path.insert(0,"$S")
from qmp import QMP
q=QMP(); q.cmd('screendump', filename=sys.argv[1], format='ppm'); q.close()
PY
sips -s format png "$S/$N.ppm" --out "$S/$N.png" >/dev/null 2>&1
echo "$S/$N.png"
