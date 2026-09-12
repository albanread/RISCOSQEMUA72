import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP

UNSHIFTED = {
    ' ':'spc', '\n':'ret', '\t':'tab',
    '-':'minus', '=':'equal', '[':'bracket_left', ']':'bracket_right',
    '\\':'backslash', ';':'semicolon', "'":'apostrophe', '`':'grave_accent',
    ',':'comma', '.':'dot', '/':'slash',
}
SHIFTED = {
    '!':'1', '"':'apostrophe', '#':'3', '$':'4', '%':'5', '^':'6', '&':'7',
    '*':'8', '(':'9', ')':'0', '_':'minus', '+':'equal', '{':'bracket_left',
    '}':'bracket_right', '|':'backslash', ':':'semicolon', '@':'2',
    '<':'comma', '>':'dot', '?':'slash', '~':'grave_accent',
}

# The guest maps a UK keyboard: fix the keys that differ from US.
UK_UNSHIFTED = {'#':'backslash', '\\':'less'}
UK_SHIFTED   = {'"':'2', '@':'apostrophe', '~':'backslash', '|':'less',
                '\u00a3':'3'}

def qcode(ch):
    """-> (qcode, needs_shift)"""
    if ch.isdigit():            return ch, False
    if 'a' <= ch <= 'z':        return ch, False
    if 'A' <= ch <= 'Z':        return ch.lower(), True
    if ch in UK_UNSHIFTED:      return UK_UNSHIFTED[ch], False
    if ch in UK_SHIFTED:        return UK_SHIFTED[ch], True
    if ch in UNSHIFTED:         return UNSHIFTED[ch], False
    if ch in SHIFTED:           return SHIFTED[ch], True
    raise ValueError(f'no qcode for {ch!r}')

def ev(key, down):
    return {'type':'key','data':{'down':down,'key':{'type':'qcode','data':key}}}

class Keyboard:
    def __init__(self, q): self.q = q
    def send(self, events): self.q.cmd('input-send-event', events=events)
    def tap(self, key, mods=(), delay=0.06):
        evs  = [ev(m, True) for m in mods] + [ev(key, True)]
        evs += [ev(key, False)] + [ev(m, False) for m in reversed(mods)]
        self.send(evs); time.sleep(delay)
    def type(self, text, delay=0.05):
        """Hold shift across runs of shifted characters: toggling it per
        key drops keystrokes in the USB HID path."""
        shift_down = False
        for ch in text:
            k, sh = qcode(ch)
            if sh != shift_down:
                self.send([ev('shift', sh)])
                shift_down = sh
                time.sleep(delay)
            self.send([ev(k, True), ev(k, False)])
            time.sleep(delay)
            if ch == '\n':
                # The task window is still running the command just sent;
                # keys that arrive meanwhile are dropped — observed losing
                # the first one or two characters of the following line.
                time.sleep(0.6)
        if shift_down:
            self.send([ev('shift', False)])
            time.sleep(delay)

if __name__ == '__main__':
    q = QMP(); kb = Keyboard(q)
    mode = sys.argv[1]
    if mode == 'type':   kb.type(sys.argv[2])
    elif mode == 'tap':  kb.tap(sys.argv[2], tuple(sys.argv[3].split(',')) if len(sys.argv)>3 and sys.argv[3] else ())
    q.close()
