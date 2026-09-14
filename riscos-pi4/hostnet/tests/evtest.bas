REM Sprint 3: the event reasons.
REM Connects out to a host listener, asks for asynchronous notification,
REM and then does nothing at all -- no polling, no reading.  Everything
REM that happens after this point happens because Internet Event 19
REM arrived.  The host sends a line, waits, and closes; the trace should
REM show async first and broken after it.
ON ERROR PROCfail(REPORT$+" at line "+STR$(ERL)) : END
DIM sa% 16, buf% 256, opt% 4
sa%?0 = 16 : sa%?1 = 2
sa%?2 = 35 : sa%?3 = 40      : REM port 9000
sa%?4 = 127 : sa%?5 = 0 : sa%?6 = 0 : sa%?7 = 1
SYS "Socket_Creat", 2, 1, 0 TO s%
opt%!0 = 1
SYS "Socket_Ioctl", s%, &8004667D, opt%   : REM FIOASYNC
SYS "Socket_Connect", s%, sa%, 16
PROClog("connected, async requested")
REM Let the events happen.  A plain wait: nothing here touches the socket.
t% = TIME
REPEAT UNTIL TIME > t% + 900
SYS "Socket_Recv", s%, buf%, 256, 0 TO n%
PROClog("recv after the wait: "+STR$(n%))
SYS "Socket_Close", s%
PROClog("done")
END
DEF PROClog(t$)
LOCAL f%
f% = OPENUP "$.evlog"
IF f% = 0 THEN f% = OPENOUT "$.evlog"
PTR#f% = EXT#f%
BPUT#f%, t$
CLOSE#f%
ENDPROC
DEF PROCfail(t$)
PROClog("ERROR: "+t$)
ENDPROC
