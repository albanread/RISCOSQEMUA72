import hashlib, os, subprocess, sys, time
S = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, S)
TARGET = hashlib.sha256(open(f'{S}/desktop.ppm','rb').read()).hexdigest()
env = dict(os.environ, RISCOS_HOSTFS=os.environ.get('RISCOS_HOSTFS', ''),
           DISPLAY_OPT='none', AUDIODEV='none')
t0 = time.time()
subprocess.Popen(['riscos-pi4/tools/run-macos.sh'], cwd=os.path.dirname(os.path.dirname(os.path.dirname(S))),
                 stdout=open(f'{S}/bt.log','w'), stderr=subprocess.STDOUT, env=env)
from qmp import QMP
q = None
while q is None:
    try: q = QMP()
    except Exception: time.sleep(0.2)
seen, first_paint = set(), None
while time.time() - t0 < 240:
    try:
        q.cmd('screendump', filename=f'{S}/bt.ppm', format='ppm')
        d = open(f'{S}/bt.ppm','rb').read()
    except Exception:
        time.sleep(0.3); continue
    h = hashlib.sha256(d).hexdigest()
    if h not in seen:
        seen.add(h)
        if first_paint is None and len(set(d[-30000:])) > 8:
            first_paint = time.time() - t0
            print(f"first real paint      {first_paint:6.1f}s")
    if h == TARGET:
        print(f"desktop reached       {time.time()-t0:6.1f}s   ({len(seen)} distinct frames)")
        break
    time.sleep(0.3)
else:
    print("did not reach the known desktop within 240s")
q.cmd('quit')
