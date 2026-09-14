REM Sprint 3: a guest listener, over HostNet.
REM Binds a port, accepts one connection, reads with Recvmsg_1 and
REM replies with Sendmsg_1 -- so the msghdr and iovec marshalling is
REM exercised, not just send and recv.  Sockets stay blocking on
REM purpose: Accept and Recvmsg then go round the module wait loop.
ON ERROR PROCfail(REPORT$+" at line "+STR$(ERL)) : END
port% = 9000
DIM sa% 16, peer% 16, plen% 4, buf% 256, opt% 4, mh% 28, iov% 8
sa%?0 = 16 : sa%?1 = 2
sa%?2 = port% DIV 256 : sa%?3 = port% MOD 256
sa%!4 = 0
SYS "Socket_Creat", 2, 1, 0 TO s%
opt%!0 = 1
SYS "Socket_Setsockopt", s%, &FFFF, 4, opt%, 4
SYS "Socket_Bind", s%, sa%, 16
SYS "Socket_Listen", s%, 1
PROClog("listening on "+STR$(port%))
plen%!0 = 16
SYS "Socket_Accept_1", s%, peer%, plen% TO c%
PROClog("accepted from "+STR$(peer%?4)+"."+STR$(peer%?5)+"."+STR$(peer%?6)+"."+STR$(peer%?7))
REM msghdr: name, namelen, iov, iovlen, control, controllen, flags
iov%!0 = buf% : iov%!4 = 256
mh%!0 = 0 : mh%!4 = 0 : mh%!8 = iov% : mh%!12 = 1
mh%!16 = 0 : mh%!20 = 0 : mh%!24 = 0
SYS "Socket_Recvmsg_1", c%, mh%, 0 TO n%
PROClog("recvmsg read "+STR$(n%)+" bytes")
IF n% <= 0 THEN PROClog("nothing to echo") : END
iov%!4 = n%
mh%!0 = 0 : mh%!4 = 0 : mh%!8 = iov% : mh%!12 = 1
mh%!16 = 0 : mh%!20 = 0 : mh%!24 = 0
SYS "Socket_Sendmsg_1", c%, mh%, 0 TO m%
PROClog("sendmsg echoed "+STR$(m%)+" bytes")
SYS "Socket_Close", c%
SYS "Socket_Close", s%
PROClog("done")
END
DEF PROClog(t$)
LOCAL f%
f% = OPENUP "$.listenlog"
IF f% = 0 THEN f% = OPENOUT "$.listenlog"
PTR#f% = EXT#f%
BPUT#f%, t$
CLOSE#f%
ENDPROC
DEF PROCfail(t$)
PROClog("ERROR: "+t$)
ENDPROC
