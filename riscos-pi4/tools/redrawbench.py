#!/usr/bin/env python3
"""Time a fixed redraw workload in the guest, from the host.

    redrawbench.py [--load|--kill] [--rounds N]

Scrolling a NetSurf page is the heaviest repeatable redraw the desktop
offers: every page down re-renders the window into its off-screen
buffer and blits the whole thing to the screen.  The workload is driven
over QMP and timed by screendumping until the picture stops changing,
so it needs nothing running in the guest and survives the guest being
slow -- which is the thing being measured.

Run it once with --kill and once with --load to compare.
"""
import argparse, json, os, socket, sys, tempfile, time

class Q:
    def __init__(self, port=4455):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=120)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, ex, **args):
        self.f.write(json.dumps({"execute": ex, "arguments": args} if args
                                else {"execute": ex}) + "\n")
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r:
                return r

    def key(self, k):
        self.cmd("send-key",
                 keys=[{"type": "qcode", "data": p} for p in k.split("-")])

    def type(self, s):
        m = {" ": "spc", ".": "dot", ":": "shift-semicolon", "$": "shift-4",
             ",": "comma", "-": "minus"}
        for ch in s:
            k = m.get(ch) or (("shift-" + ch.lower()) if ch.isupper() else ch)
            self.key(k)
            time.sleep(0.12 if "-" in k else 0.06)

    def click(self, x, y, w=1920, h=1200):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x*32767/w)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y*32767/h)}}])
        time.sleep(0.15)
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": False, "button": "left"}}])

    def press(self, x, y, w=1920, h=1200):
        self.move(x, y, w, h)
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": True, "button": "left"}}])

    def release(self):
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": False, "button": "left"}}])

    def move(self, x, y, w=1920, h=1200):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x*32767/w)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y*32767/h)}}])

    def shot(self, path):
        try:
            os.unlink(path)
        except OSError:
            pass
        self.cmd("screendump", filename=path)
        with open(path, "rb") as fh:
            return fh.read()


def settle(q, tmp, quiet_for=2, poll=0.10, limit=60.0):
    """Seconds until the screen is unchanged across `quiet_for` dumps."""
    t0 = time.time()
    same = 0
    prev = None
    while time.time() - t0 < limit:
        cur = q.shot(tmp)
        if prev is not None and cur == prev:
            same += 1
            if same >= quiet_for:
                return time.time() - t0
        else:
            same = 0
        prev = cur
        time.sleep(poll)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--load", action="store_true", help="load GVFill first")
    ap.add_argument("--kill", action="store_true", help="unload GVFill first")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--pages", type=int, default=12)
    ap.add_argument("--sx", type=int, default=1608, help="scrollbar x")
    ap.add_argument("--up", type=int, default=32, help="up arrow y")
    ap.add_argument("--down", type=int, default=1068, help="down arrow y")
    ap.add_argument("--drag", action="store_true",
                    help="drag a window instead of scrolling: pure redraw, "
                         "no page layout, so the blit cost is not buried")
    ap.add_argument("--dragx", type=int, default=200)
    ap.add_argument("--dragy", type=int, default=651)
    a = ap.parse_args()

    q = Q()
    tmp = os.path.join(tempfile.gettempdir(), "redrawbench.ppm")

    if a.load or a.kill:
        q.key("f12"); time.sleep(1.2)
        q.type("rmkill GVFill"); q.key("ret"); time.sleep(1.5)
        if a.load:
            q.type("rmload hostfs:$.GVFill,ffa"); q.key("ret"); time.sleep(2.5)
        q.key("ret"); time.sleep(4.0)

    best = None
    for r in range(a.rounds):
        if a.drag:
            # Dragging a window over another forces the one underneath to
            # redraw from its buffer, again and again, with no layout to
            # hide behind.
            settle(q, tmp)
            t0 = time.time()
            q.press(a.dragx, a.dragy)
            for sweep in range(2):
                xs = range(a.dragx, a.dragx + 700, 25)
                for x in (xs if sweep == 0 else reversed(list(xs))):
                    q.move(x, a.dragy)
                    time.sleep(0.02)
            q.release()
            el = settle(q, tmp)
            if el is None:
                print("round %d: did not settle" % (r + 1))
                continue
            total = time.time() - t0
            print("round %d: %.2fs" % (r + 1, total))
            best = total if best is None else min(best, total)
            continue

        # back to the top, and let everything go quiet before timing.
        # The scroll bar rather than the keyboard: the Wimp gives key
        # events to whichever window holds input focus, which is not
        # something a benchmark should have to fight for.
        for _ in range(a.pages + 6):
            q.click(a.sx, a.up); time.sleep(0.05)
        settle(q, tmp)

        t0 = time.time()
        for _ in range(a.pages):
            q.click(a.sx, a.down); time.sleep(0.05)
        el = settle(q, tmp)
        if el is None:
            print("round %d: did not settle" % (r + 1))
            continue
        total = time.time() - t0
        print("round %d: %.2fs" % (r + 1, total))
        best = total if best is None else min(best, total)

    if best is None:
        sys.exit("no rounds completed")
    print("best: %.2fs for %d page scrolls" % (best, a.pages))


main()
