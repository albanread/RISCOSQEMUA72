REM >VMTest -- virtual memory mapping tests for the QEMU Raspberry Pi 4 port
REM
REM Kept unnumbered here; run.py numbers it (so BASIC never stops to say
REM "Program renumbered"), copies it to the test disc and runs it from a
REM boot task.  Parameters come from system variables the task sets:
REM   VMT$Test    which test -- see the CASE below
REM   VMT$Target  directory for the test's files, e.g. RAM::RamDisc0.$
REM   VMT$Size    buffer size in KB
REM   VMT$RamFS   RAM disc size in MB to ask for first (0 leaves it alone)
REM   VMT$Rounds  rounds for the stress test
REM Every step is logged as one closed line to HostFS:$.VMTest.log, so a
REM machine that dies still shows the step it died in.  An error records
REM *ShowRegs and *Where beside the log.
DIM vbuf% 256
failing% = FALSE
log$ = "HostFS:$.VMTest.log"
ON ERROR PROCfail : END
test$ = FNvar("VMT$Test", "svcread")
target$ = FNvar("VMT$Target", "RAM::RamDisc0.$")
kb% = VAL(FNvar("VMT$Size", "16384"))
ramfs% = VAL(FNvar("VMT$RamFS", "0"))
rounds% = VAL(FNvar("VMT$Rounds", "2"))
PROCasm
SYS "OS_GetEnv" TO , slotend%
PROClog("start "+test$+" target="+target$+" size="+STR$(kb%)+"K lomem=&"+STR$~LOMEM+" himem=&"+STR$~HIMEM+" slotend=&"+STR$~slotend%)
IF ramfs% > 0 THEN PROCramfs(ramfs%)
CASE test$ OF
  WHEN "svcread" : PROCsvcread
  WHEN "svcwrite" : PROCsvcwrite
  WHEN "usrtouch" : PROCusrtouch
  WHEN "ramstress" : PROCramstress
  WHEN "spritegrab" : PROCspritegrab
  WHEN "spritecopy" : PROCspritecopy
  OTHERWISE : PROClog("unknown test "+test$)
ENDCASE
PROClog("end ok")
END

REM Save a buffer this program has never touched.  The OS_File 10 copy
REM reads every page in SVC mode, so each page is first reached from SVC.
DEF PROCsvcread
LOCAL buf%, size%, f$
size% = kb% * 1024
DIM buf% size% - 1
f$ = target$ + ".svcread"
PROClog("svcread: untouched buffer &"+STR$~buf%+" + &"+STR$~size%+", OS_File 10 to "+f$)
SYS "OS_File", 10, f$, &FFD, , buf%, buf% + size%
PROClog("svcread: saved, length "+STR$(FNlength(f$)))
ENDPROC

REM Load a file into a buffer this program has never touched: the load
REM writes every page in SVC mode.  The source is written from a buffer
REM filled (so touched) in user mode, and the result is checked.
DEF PROCsvcwrite
LOCAL src%, dst%, size%, f$, bad%
size% = kb% * 1024
DIM src% size% - 1
A% = src% : B% = size% : C% = &5EED0001 : CALL fill%
f$ = target$ + ".svcwrite"
SYS "OS_File", 10, f$, &FFD, , src%, src% + size%
PROClog("svcwrite: wrote a patterned file of "+STR$(size%)+" bytes")
DIM dst% size% - 1
PROClog("svcwrite: untouched buffer &"+STR$~dst%+", OS_File 16 from "+f$)
SYS "OS_File", 16, f$, dst%, 0
A% = dst% : B% = size% : C% = &5EED0001
bad% = USR check%
PROClog("svcwrite: loaded, check "+FNcheckmsg(bad%))
ENDPROC

REM The control for svcread: touch one byte of every page from user mode
REM first, as the WimpForth workaround does, then save.
DEF PROCusrtouch
LOCAL buf%, size%, i%, s%, f$
size% = kb% * 1024
DIM buf% size% - 1
FOR i% = 0 TO size% - 1 STEP 4096 : s% = buf%?i% : NEXT
s% = buf%?(size% - 1)
f$ = target$ + ".usrtouch"
PROClog("usrtouch: read every page of &"+STR$~buf%+" in USR, OS_File 10 to "+f$)
SYS "OS_File", 10, f$, &FFD, , buf%, buf% + size%
PROClog("usrtouch: saved, length "+STR$(FNlength(f$)))
ENDPROC

REM Many large patterned files until the disc is full, each saved and read
REM back and checked, then every file checked again before the next round
REM (catches data that moves when the area grows), then all deleted.
DEF PROCramstress
LOCAL buf%, rbuf%, size%, r%, n%, k%, f$, bad%, e%, fl%, t%, stop%
size% = kb% * 1024
DIM buf% size% - 1, rbuf% size% - 1
FOR r% = 1 TO rounds%
  n% = 0 : stop% = FALSE
  REPEAT
    f$ = target$ + ".s" + STR$(r%) + "_" + STR$(n% + 1)
    A% = buf% : B% = size% : C% = r% * 65536 + n% + 1 : CALL fill%
    t% = TIME
    SYS "XOS_File", 10, f$, &FFD, , buf%, buf% + size% TO e% ;fl%
    IF fl% AND 1 THEN
      PROClog("round "+STR$(r%)+": file "+STR$(n% + 1)+" not saved: "+FNerr(e%))
      stop% = TRUE
    ELSE
      n% += 1
      SYS "OS_File", 16, f$, rbuf%, 0
      A% = rbuf% : B% = size% : C% = r% * 65536 + n%
      bad% = USR check%
      PROClog("round "+STR$(r%)+": file "+STR$(n%)+" saved and read back, check "+FNcheckmsg(bad%)+", "+STR$(TIME - t%)+"cs")
      IF bad% THEN stop% = TRUE
    ENDIF
  UNTIL stop% OR n% >= 1000
  FOR k% = 1 TO n%
    f$ = target$ + ".s" + STR$(r%) + "_" + STR$(k%)
    SYS "OS_File", 16, f$, rbuf%, 0
    A% = rbuf% : B% = size% : C% = r% * 65536 + k%
    bad% = USR check%
    IF bad% THEN PROClog("round "+STR$(r%)+": recheck file "+STR$(k%)+" "+FNcheckmsg(bad%))
  NEXT
  PROClog("round "+STR$(r%)+": "+STR$(n%)+" files rechecked")
  FOR k% = 1 TO n%
    SYS "XOS_File", 6, target$ + ".s" + STR$(r%) + "_" + STR$(k%)
  NEXT
  PROClog("round "+STR$(r%)+": files deleted")
NEXT
ENDPROC

REM The kernel's pixel copies into application space -- the WimpForth
REM abort was an SVC STM of pixels to an untouched page.  Grab the screen
REM into a sprite in a buffer this program has never touched, then plot
REM it back (a plot GVFill may accelerate, reading the sprite on the host).
DEF PROCspritegrab
LOCAL area%, size%
size% = kb% * 1024
DIM area% size% - 1
PROCspritearea(area%, size%)
PROClog("spritegrab: area &"+STR$~area%+" + &"+STR$~size%+", OS_SpriteOp 16 (grab the screen)")
SYS "OS_SpriteOp", &110, area%, "grab", 0, 0, 0, 1599, 1199
PROClog("spritegrab: grabbed, "+FNspriteinfo(area%, "grab"))
SYS "OS_SpriteOp", &122, area%, "grab", 0, 0, 0
PROClog("spritegrab: plotted back to the screen")
ENDPROC

REM Application space to application space: grab the screen, create a
REM second sprite, redirect output into it and plot the first sprite into
REM it -- the kernel (or GVFill) copies pixels from one untouched region
REM of the slot into another.
DEF PROCspritecopy
LOCAL area%, size%, w%, h%, r0%, r1%, r2%, r3%
size% = kb% * 1024
DIM area% size% - 1
PROCspritearea(area%, size%)
SYS "OS_SpriteOp", &110, area%, "grab", 0, 0, 0, 1599, 1199
SYS "OS_SpriteOp", &128, area%, "grab" TO , , , w%, h%
PROClog("spritecopy: grabbed "+STR$(w%)+"x"+STR$(h%)+" into &"+STR$~area%)
SYS "OS_SpriteOp", &10F, area%, "dst", 0, w%, h%, &301680B5
PROClog("spritecopy: created dst, "+FNspriteinfo(area%, "dst"))
SYS "OS_SpriteOp", &13C, area%, "dst", 0 TO r0%, r1%, r2%, r3%
SYS "OS_SpriteOp", &122, area%, "grab", 0, 0, 0
SYS "OS_SpriteOp", r0%, r1%, r2%, r3%
PROClog("spritecopy: plotted grab into dst and restored output")
ENDPROC

DEF PROCspritearea(area%, size%)
!area% = size%
area%!8 = 16
SYS "OS_SpriteOp", &109, area%
ENDPROC

DEF FNspriteinfo(area%, n$)
LOCAL w%, h%, m%
SYS "OS_SpriteOp", &128, area%, n$ TO , , , w%, h%, , m%
="sprite "+n$+" "+STR$(w%)+"x"+STR$(h%)+" mode &"+STR$~m%+", area used &"+STR$~(area%!12)

DEF PROCramfs(mb%)
PROClog("RAM disc: asking for "+STR$(mb%)+"MB")
OSCLI "ChangeDynamicArea -RamFsSize "+STR$(mb%)+"M"
PROClog("RAM disc: done")
ENDPROC

DEF FNlength(f$)
LOCAL len%
SYS "OS_File", 17, f$ TO , , , , len%
=len%

DEF FNvar(name$, def$)
LOCAL len%, fl%
SYS "XOS_ReadVarVal", name$, vbuf%, 255, 0, 3 TO , , len% ;fl%
IF (fl% AND 1) <> 0 OR len% = 0 THEN =def$
vbuf%?len% = 13
=$vbuf%

DEF FNcstr(p%)
LOCAL s$
WHILE ?p% <> 0 AND LEN(s$) < 200 : s$ += CHR$(?p%) : p% += 1 : ENDWHILE
=s$

DEF FNerr(e%) = FNcstr(e% + 4) + " (&" + STR$~(!e%) + ")"

DEF FNcheckmsg(b%)
IF b% = 0 THEN ="ok"
="MISMATCH at offset &"+STR$~(b% - 1)

DEF PROClog(t$)
LOCAL f%
f% = OPENUP(log$)
IF f% = 0 THEN f% = OPENOUT(log$)
PTR#f% = EXT#f%
BPUT#f%, STR$(TIME) + " " + t$
CLOSE#f%
ENDPROC

DEF PROCfail
IF failing% THEN END
failing% = TRUE
PROClog("ERROR "+REPORT$+" (&"+STR$~ERR+") at line "+STR$(ERL))
OSCLI "ShowRegs { > HostFS:$.VMTest.regs }"
OSCLI "Where { > HostFS:$.VMTest.where }"
PROClog("regs and where recorded")
ENDPROC

REM fill% (R0 buffer, R1 length, R2 seed) writes word k as k*&9E3779B1+seed,
REM a tail of 1-3 bytes taken from the next word; check% (same arguments)
REM returns 0, or 1 + the offset of the first byte that differs.  The
REM pattern depends on position, so data landing in the wrong page shows.
DEF PROCasm
LOCAL pass%
DIM code% 512
FOR pass% = 0 TO 2 STEP 2
P% = code%
[OPT pass%
.fill%
STMFD R13!, {R4-R6, R14}
MOV R3, #0
LDR R4, mul
.fill_w
SUBS R1, R1, #4
BLT fill_t
MUL R5, R3, R4
ADD R5, R5, R2
STR R5, [R0], #4
ADD R3, R3, #1
B fill_w
.fill_t
ADDS R1, R1, #4
BEQ fill_x
MUL R5, R3, R4
ADD R5, R5, R2
.fill_b
STRB R5, [R0], #1
MOV R5, R5, LSR #8
SUBS R1, R1, #1
BNE fill_b
.fill_x
MOV R0, #0
LDMFD R13!, {R4-R6, PC}
.mul
EQUD &9E3779B1
.check%
STMFD R13!, {R4-R7, R14}
MOV R6, R0
MOV R3, #0
LDR R4, mul
.chk_w
SUBS R1, R1, #4
BLT chk_t
MUL R5, R3, R4
ADD R5, R5, R2
LDR R7, [R0], #4
CMP R7, R5
BNE chk_bad4
ADD R3, R3, #1
B chk_w
.chk_t
ADDS R1, R1, #4
BEQ chk_ok
MUL R5, R3, R4
ADD R5, R5, R2
.chk_b
LDRB R7, [R0], #1
AND R14, R5, #&FF
CMP R7, R14
BNE chk_bad1
MOV R5, R5, LSR #8
SUBS R1, R1, #1
BNE chk_b
.chk_ok
MOV R0, #0
LDMFD R13!, {R4-R7, PC}
.chk_bad4
SUB R0, R0, #3
SUB R0, R0, R6
LDMFD R13!, {R4-R7, PC}
.chk_bad1
SUB R0, R0, R6
LDMFD R13!, {R4-R7, PC}
]
NEXT
ENDPROC
