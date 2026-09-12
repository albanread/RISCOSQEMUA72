import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp import QMP
W, H = 800, 600
def abs_ev(x, y):
    return [{'type':'abs','data':{'axis':'x','value':int(x*32767/W)}},
            {'type':'abs','data':{'axis':'y','value':int(y*32767/H)}}]
def btn(name, down):
    return {'type':'btn','data':{'down':down,'button':name}}
if __name__ == '__main__':
    q = QMP()
    x, y = int(sys.argv[1]), int(sys.argv[2])
    button = sys.argv[3] if len(sys.argv) > 3 else 'left'
    q.cmd('input-send-event', events=abs_ev(x, y)); time.sleep(0.2)
    if button != 'move':
        q.cmd('input-send-event', events=[btn(button, True)]); time.sleep(0.1)
        q.cmd('input-send-event', events=[btn(button, False)])
    q.close()
