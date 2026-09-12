import hashlib, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP

S = os.path.dirname(os.path.abspath(__file__))
q = QMP()
t0 = time.time()
last, stable_since, shots = None, None, 0
SETTLE = 3.0          # image unchanged this long == settled
DEADLINE = 180.0
while time.time() - t0 < DEADLINE:
    p = f"{S}/shot.ppm"
    try:
        q.cmd('screendump', filename=p, format='ppm')
    except Exception as e:
        print(f"[{time.time()-t0:6.1f}s] screendump failed: {e}"); time.sleep(2); continue
    data = open(p,'rb').read()
    # PPM header: P6\n<w> <h>\n255\n
    hdr = data[:32].split(b'\n')
    dims = hdr[1].decode() if len(hdr) > 1 else '?'
    h = hashlib.sha256(data).hexdigest()[:12]
    shots += 1
    if h != last:
        print(f"[{time.time()-t0:6.1f}s] {dims:>10}  {len(data):>9} B  {h}  CHANGED")
        last, stable_since = h, time.time()
    else:
        if stable_since and time.time() - stable_since >= SETTLE:
            print(f"[{time.time()-t0:6.1f}s] {dims:>10}  settled for {SETTLE:.0f}s -> desktop is up")
            os.replace(p, f"{S}/desktop.ppm")
            q.close(); sys.exit(0)
    time.sleep(1.0)
print("DEADLINE reached without settling"); q.close(); sys.exit(1)
