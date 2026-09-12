import hashlib, os, sys, time
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
from qmp import QMP
TARGET = hashlib.sha256(open(f'{S}/desktop.ppm','rb').read()).hexdigest()
t0 = time.time(); q = None
while q is None and time.time()-t0 < 60:
    try: q = QMP()
    except Exception: time.sleep(0.3)
while time.time() - t0 < 240:
    try:
        q.cmd('screendump', filename=f'{S}/w.ppm', format='ppm')
        if hashlib.sha256(open(f'{S}/w.ppm','rb').read()).hexdigest() == TARGET:
            print(f"desktop up at {time.time()-t0:.1f}s"); q.close(); sys.exit(0)
    except Exception: pass
    time.sleep(0.5)
print("timeout"); sys.exit(1)
