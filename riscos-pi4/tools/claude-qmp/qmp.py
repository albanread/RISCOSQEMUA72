import json, os, socket, sys, time

class QMP:
    def __init__(self, host='127.0.0.1', port=4455, timeout=60):
        # QMP_SOCK names an instance's unix socket (tools/instance.sh qmp
        # <name>).  Without it this falls back to the old shared TCP port,
        # which only one machine on the host can own.
        sock = os.environ.get('QMP_SOCK')
        if sock:
            self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.s.settimeout(timeout)
            self.s.connect(sock)
        else:
            self.s = socket.create_connection((host, port), timeout=timeout)
        self.f = self.s.makefile('rwb')
        self.greeting = self._read()          # QMP greeting
        self.cmd('qmp_capabilities')
    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError('QMP closed')
            msg = json.loads(line)
            if 'event' in msg:                 # skip async events
                continue
            return msg
    def cmd(self, name, **args):
        req = {'execute': name}
        if args:
            req['arguments'] = args
        self.f.write((json.dumps(req) + '\r\n').encode())
        self.f.flush()
        r = self._read()
        if 'error' in r:
            raise RuntimeError(f"{name}: {r['error']['class']}: {r['error']['desc']}")
        return r.get('return')
    def close(self):
        try: self.f.close(); self.s.close()
        except Exception: pass

if __name__ == '__main__':
    q = QMP()
    print(json.dumps(q.cmd(sys.argv[1], **(json.loads(sys.argv[2]) if len(sys.argv) > 2 else {})), indent=2))
    q.close()
