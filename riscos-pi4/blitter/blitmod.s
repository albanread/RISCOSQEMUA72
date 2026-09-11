@ GVFill -- RISC OS module: rectangle fill through the host blitter.
@
@ Pure assembly with no absolute relocations, so it assembles and is
@ turned into a module image with clang and llvm-objcopy alone; the
@ roscc module linker the HostFS build uses is Windows-only and is not
@ needed when there is no C and nothing static-base relative.
@
@ Storage lives in the module body.  A soft-loaded module is in the RMA,
@ which is writable and, like all RAM on RISC OS 5, identity mapped --
@ so the descriptor's logical address is the physical one the device
@ needs, with no OS_Memory translation.

    .arch   armv8-a
    .arm
    .text
    .global _start

    .equ    BLIT_PHYS,   0xFD404000     @ physical, mapped at init
    .equ    BLIT_MAGIC,  0x54494C42     @ 'BLIT'
    .equ    BLIT_GO,      0x0C
    .equ    BLIT_OP,      0x10
    .equ    BLIT_FLAGS,   0x14
    .equ    BLIT_DEST,    0x18
    .equ    BLIT_SRC,     0x1C
    .equ    BLIT_WIDTH,   0x20
    .equ    BLIT_HEIGHT,  0x24
    .equ    BLIT_DSTRIDE, 0x28
    .equ    BLIT_SSTRIDE, 0x2C
    .equ    BLIT_PATTERN, 0x30
    .equ    BLIT_PATLEN,  0x40

    .equ    OP_FILL,   1
    .equ    F_FB,      1

    .equ    XOS_WriteS,            0x20001
    .equ    XOS_Write0,            0x20002
    .equ    XOS_NewLine,           0x20003
    .equ    XOS_ReadVduVariables,  0x20031
    .equ    XOS_ConvertHex8,       0x200D4
    .equ    XOS_Memory,            0x20068
    .equ    MapIOPermanent,        13
    .equ    XOS_Claim,             0x2001F
    .equ    XOS_Release,           0x20020
    .equ    XOS_WriteI,            0x20100

    .equ    GraphicsV,             0x2A
    .equ    GraphicsV_Render,      13
    .equ    GVRender_FillRectangle, 2
    .equ    GraphicsV_Complete,    0

    .equ    SpriteV,               0x1F
    .equ    spWidth,               16      @ width in words - 1
    .equ    spHeight,              20      @ height in rows - 1
    .equ    spImage,               32
    .equ    spTrans,               36      @ == spImage when unmasked
    .equ    spLBit,                24      @ first bit used
    .equ    spRBit,                28      @ last bit used
    .equ    spMode,                40

    .equ    BLIT_DSTX,    0x44
    .equ    BLIT_DSTY,    0x48
    .equ    BLIT_CLIPX0,  0x4C
    .equ    BLIT_CLIPY0,  0x50
    .equ    BLIT_CLIPX1,  0x54
    .equ    BLIT_CLIPY1,  0x58
    .equ    BLIT_BPP,     0x5C
    .equ    OP_SPRITE,    3
    .equ    F_SRC_VIRT,   2
    .equ    F_BOTTOM_UP,  4
    .equ    XOS_SpriteOp,          0x2002E
    .equ    XOS_ReadMonotonicTime, 0x20042
    .equ    XOS_WriteC,            0x20000
    .equ    XOS_Module,            0x2001E
    .equ    ModClaim,              6

    .equ    BENCH_W,      512             @ pixels, one word each
    .equ    BENCH_H,      512
    .equ    BENCH_IMG,    BENCH_W * BENCH_H * 4
    .equ    BENCH_AREA,   16 + 44 + BENCH_IMG
    .equ    BENCH_PLOTS,  1000

_start:
base:
    .word   0                       @ start
    .word   init    - base
    .word   final   - base
    .word   0                       @ service call handler
    .word   title   - base
    .word   help    - base
    .word   cmdtab  - base
    .word   0                       @ SWI chunk
    .word   0                       @ SWI handler
    .word   0                       @ SWI decoding table
    .word   0                       @ SWI decoding code
    .word   0                       @ messages file
    .word   modflags - base

title:
    .asciz  "GVFill"
help:
    .asciz  "GVFill\t1.00 (11 Sep 2026) Host-blitter rectangle fill"
    .balign 4
modflags:
    .word   1                       @ 32-bit compatible

cmdtab:
    .asciz  "BlitFill"
    .balign 4
    .word   cmd_blitfill - base
    .word   0                       @ no parameters
    .word   cmd_syntax - base
    .word   cmd_help   - base
    .asciz  "BlitStats"
    .balign 4
    .word   cmd_blitstats - base
    .word   0
    .word   st_syntax - base
    .word   st_help   - base
    .asciz  "SprStats"
    .balign 4
    .word   cmd_sprstats - base
    .word   0
    .word   sv_syntax - base
    .word   sv_help   - base
    .asciz  "SprBench"
    .balign 4
    .word   cmd_sprbench - base
    .word   0
    .word   sb_syntax - base
    .word   sb_help   - base
    .byte   0                       @ end of table
    .balign 4
st_syntax:
    .asciz  "Syntax:\t*BlitStats"
st_help:
    .asciz  "*BlitStats reports the FillRectangle calls seen on GraphicsV.\r"
    .balign 4
sv_syntax:
    .asciz  "Syntax:\t*SprStats"
sv_help:
    .asciz  "*SprStats counts the sprite plots the desktop asks for.\r"
sb_syntax:
    .asciz  "Syntax:\t*SprBench"
sb_help:
    .asciz  "*SprBench replots the last big sprite, host and guest, and times both.\r"
    .balign 4
cmd_syntax:
    .asciz  "Syntax:\t*BlitFill"
cmd_help:
    .asciz  "*BlitFill paints a test rectangle with the host blitter.\r"
    .balign 4

@ ---------------------------------------------------------------- init
@ The device page has to be mapped before it can be touched: RISC OS
@ builds its logical map from what the HAL asks for, so an address in
@ the peripheral window that nothing claimed simply aborts.
init:
    STMFD   sp!, {r1-r4, lr}
    MOV     r0, #MapIOPermanent
    LDR     r1, =BLIT_PHYS
    MOV     r2, #0x1000
    SWI     XOS_Memory
    BVS     init_fail
    ADR     r1, blit_log
    STR     r3, [r1]                @ logical address of the device
    LDR     r2, [r3]
    LDR     r4, =BLIT_MAGIC
    TEQ     r2, r4
    BNE     init_nodev
    MOV     r0, #GraphicsV
    ADR     r1, gv_handler
    MOV     r2, #0
    SWI     XOS_Claim
    BVS     init_fail
    MOV     r0, #SpriteV
    ADR     r1, sv_veneer
    MOV     r2, #0
    SWI     XOS_Claim
    BVS     init_fail
    MSR     CPSR_f, #0              @ V clear: no error
    LDMFD   sp!, {r1-r4, pc}
init_nodev:
    ADR     r0, err_nodev
init_fail:
    LDMFD   sp!, {r1-r4, lr}
    MSR     CPSR_f, #(1 << 28)      @ V set
    MOV     pc, lr

err_nodev:
    .word   0x00800100
    .asciz  "No host blitter at this address"
    .balign 4

sv_veneer:
    B       sv_handler

final:
    STMFD   sp!, {r0-r2, lr}
    MOV     r0, #SpriteV
    ADR     r1, sv_veneer
    MOV     r2, #0
    SWI     XOS_Release
    MOV     r0, #GraphicsV
    ADR     r1, gv_handler
    MOV     r2, #0
    SWI     XOS_Release
    LDMFD   sp!, {r0-r2, lr}
    MSR     CPSR_f, #0
    MOV     pc, lr

@ ------------------------------------------------- GraphicsV handler
@ Records the first FillRectangle and passes every call on untouched:
@ r4 is left alone, so the kernel still does the fill itself.  The two
@ call sites disagree about the colour block -- vdugrafa passes GColAdr,
@ an eight-word ECF, and vduwrch builds a sixteen-word interleaved
@ ora/eor block -- and since no driver has ever implemented this
@ operation, the only way to learn which arrives is to look.
gv_handler:
    STMFD   sp!, {r0-r3, r5-r11, lr}    @ not r4: the answer goes there
    AND     r5, r4, #0xFF
    TEQ     r5, #GraphicsV_Render
    BNE     gv_pass
    TEQ     r1, #GVRender_FillRectangle
    BNE     gv_pass

    @ The colour block is sixteen words of interleaved (ora, eor), from
    @ both call sites -- vduwrch builds one on the stack and vdugrafa
    @ passes GColAdr, which points at the kernel's OraEor block despite
    @ what hdr/KernelWS calls it.  A plain colour is every ora all ones,
    @ which reduces (dest ORR ora) EOR eor to a constant; any other GCOL
    @ action needs the destination read, so leave those to the kernel.
    LDR     r6, [r2, #16]
    LDR     r9, [r6, #4]
    MVN     r9, r9                  @ r9 = the pixel word
    MOV     r8, #8
gv_chk:
    LDR     r0, [r6], #4
    CMN     r0, #1                  @ EQ only if 0xFFFFFFFF
    BNE     gv_pass
    LDR     r0, [r6], #4
    MVN     r0, r0
    TEQ     r0, r9
    BNE     gv_pass
    SUBS    r8, r8, #1
    BNE     gv_chk

    @ Screen geometry.  These are mode constants, not per-plot state,
    @ so reading them here is safe even mid-plot.
    ADR     r0, vduvars
    ADR     r1, vduvals
    SWI     XOS_ReadVduVariables
    BVS     gv_pass
    ADR     r6, vduvals
    LDR     r5, [r6, #0]            @ LineLength
    LDR     r7, [r6, #4]            @ Log2BPP
    LDR     r11, [r6, #12]          @ YWindLimit = yres - 1
    CMP     r7, #3                  @ sub-byte pixels are not whole bytes
    BLO     gv_pass
    SUB     r7, r7, #3
    MOV     r8, #1
    MOV     r8, r8, LSL r7          @ r8 = bytes per pixel

    @ Coordinates are inclusive pixels with the origin at the bottom
    @ left, so the first row in memory is the top one and the rows
    @ descend by one pitch.
    LDMIA   r2, {r0, r1, r3, r6}    @ left, top, right, bottom
    SUB     r3, r3, r0
    ADD     r3, r3, #1
    MUL     r3, r8, r3              @ r3 = width in bytes
    SUB     r6, r1, r6
    ADD     r6, r6, #1              @ r6 = height in rows
    SUB     r11, r11, r1            @ rows down to the top of the rectangle
    MUL     r10, r5, r11
    MLA     r11, r8, r0, r10        @ r11 = byte offset of the top left

    ADR     r0, blit_log
    LDR     r0, [r0]
    TEQ     r0, #0
    BEQ     gv_pass

    MOV     r1, #OP_FILL
    STR     r1, [r0, #BLIT_OP]
    MOV     r1, #F_FB
    STR     r1, [r0, #BLIT_FLAGS]
    STR     r11, [r0, #BLIT_DEST]
    STR     r3, [r0, #BLIT_WIDTH]
    STR     r6, [r0, #BLIT_HEIGHT]
    STR     r5, [r0, #BLIT_DSTRIDE]
    MOV     r1, #4
    STR     r1, [r0, #BLIT_PATLEN]
    STR     r9, [r0, #BLIT_PATTERN]
    STR     r9, [r0, #BLIT_PATTERN + 4]
    STR     r9, [r0, #BLIT_PATTERN + 8]
    STR     r9, [r0, #BLIT_PATTERN + 12]
    STR     r9, [r0, #BLIT_GO]

    LDR     r1, [r0, #BLIT_GO]      @ status
    TEQ     r1, #0
    BNE     gv_pass                 @ refused: let the kernel do it

    ADR     r0, obs_count
    LDR     r1, [r0]
    ADD     r1, r1, #1
    STR     r1, [r0]

    MOV     r4, #GraphicsV_Complete
gv_pass:
    LDMFD   sp!, {r0-r3, r5-r11, pc}

@ ----------------------------------------------------------- *BlitStats
cmd_blitstats:
    STMFD   sp!, {r0-r8, lr}
    ADR     r6, obs_count
    MOV     r7, #1                  @ rectangles accelerated
    MOV     r8, #0
st_loop:
    LDR     r0, [r6], #4
    ADR     r1, hexbuf
    MOV     r2, #12
    SWI     XOS_ConvertHex8
    BVS     st_out
    SWI     XOS_Write0
    SWI     XOS_WriteI + 32
    ADD     r8, r8, #1
    TEQ     r8, #6
    MOVEQ   r8, #0
    SWIEQ   XOS_NewLine
    SUBS    r7, r7, #1
    BNE     st_loop
    SWI     XOS_NewLine
st_out:
    MSR     CPSR_f, #0
    LDMFD   sp!, {r0-r8, pc}

obs_count:
    .word   0


@ ------------------------------------------------------------ *BlitFill
@ Paints 200x100 pixels at the top-left of the screen.
cmd_blitfill:
    STMFD   sp!, {r0-r11, lr}

    ADR     r0, vduvars
    ADR     r1, vduvals
    SWI     XOS_ReadVduVariables
    BVS     cmd_out

    ADR     r4, vduvals
    LDR     r5, [r4, #0]            @ r5 = LineLength (pitch)
    LDR     r6, [r4, #4]            @ r6 = Log2BPP

    CMP     r6, #3                  @ sub-byte pixels cannot be filled
    BLO     cmd_deep                @ by whole bytes
    SUB     r6, r6, #3
    MOV     r7, #1
    MOV     r7, r7, LSL r6          @ r7 = bytes per pixel

    ADR     r1, blit_log
    LDR     r1, [r1]                @ r1 = device, logical

    MOV     r0, #OP_FILL
    STR     r0, [r1, #BLIT_OP]
    MOV     r0, #F_FB
    STR     r0, [r1, #BLIT_FLAGS]
    MOV     r0, #0
    STR     r0, [r1, #BLIT_DEST]    @ top-left of the screen
    STR     r0, [r1, #BLIT_SRC]
    STR     r0, [r1, #BLIT_SSTRIDE]
    MOV     r0, #200
    MUL     r0, r7, r0
    STR     r0, [r1, #BLIT_WIDTH]
    MOV     r0, #100
    STR     r0, [r1, #BLIT_HEIGHT]
    STR     r5, [r1, #BLIT_DSTRIDE]
    MOV     r0, #4
    STR     r0, [r1, #BLIT_PATLEN]

    LDR     r0, =0x00FF00FF         @ unmistakable magenta
    STR     r0, [r1, #BLIT_PATTERN]
    STR     r0, [r1, #BLIT_PATTERN + 4]
    STR     r0, [r1, #BLIT_PATTERN + 8]
    STR     r0, [r1, #BLIT_PATTERN + 12]

    STR     r0, [r1, #BLIT_GO]      @ any value: run it

    LDR     r0, [r1, #BLIT_GO]      @ status of that blit
    ADR     r1, hexbuf
    MOV     r2, #12
    SWI     XOS_ConvertHex8
    BVS     cmd_out
    MOV     r1, r0
    SWI     XOS_WriteS
    .asciz  "blitter status "
    .balign 4
    MOV     r0, r1
    SWI     XOS_Write0
    SWI     XOS_NewLine
cmd_out:
    MSR     CPSR_f, #0
    LDMFD   sp!, {r0-r11, pc}

cmd_deep:
    ADR     r0, err_deep
    LDMFD   sp!, {r1-r11, lr}
    ADD     sp, sp, #4
    MSR     CPSR_f, #(1 << 28)
    MOV     pc, lr

err_deep:
    .word   0x00800101
    .asciz  "Screen depth is below 8bpp"
    .balign 4

vduvars:
    .word   6                       @ LineLength
    .word   9                       @ Log2BPP
    .word   11                      @ XWindLimit
    .word   12                      @ YWindLimit
    .word   -1
vduvals:
    .word   0, 0, 0, 0
blit_log:
    .word   0
hexbuf:
    .space  16
    .balign 4

@ --------------------------------------------------- SpriteV observer
@ Counts the plotting reasons and passes every call on untouched.  The
@ kernel only takes its internal fast path when it is the sole owner of
@ SpriteV, so claiming this sends every sprite op down the vector --
@ which is the point, but also means the handler must be cheap and must
@ leave every register alone.
@
@ It lives at the end with its data beside it: ADR reaches about a
@ kilobyte, and a module with no relocations has nothing else to address
@ with.
sv_handler:
    STMFD   sp!, {r0-r11, lr}
    AND     r10, r0, #0xFF
    TEQ     r10, #52                @ PutSpriteScaled: the only one that pays
    BNE     sv_count

    ADR     r11, sv_n52
    LDR     r8, [r11]
    ADD     r8, r8, #1
    STR     r8, [r11]

    @ The case worth taking: the sprite pointed at rather than named,
    @ unmasked, already the screen's depth, whole words edge to edge,
    @ plain store, and not really scaling.  A pixel translation table is
    @ allowed through: at 32bpp it is a ColourMap format descriptor, not
    @ a palette, and for a sprite already in the screen's format it has
    @ nothing to say.
    CMP     r0, #512
    BLO     sv_pass
    LDR     r8, [r2, #spImage]
    LDR     r9, [r2, #spTrans]
    TEQ     r8, r9
    BNE     sv_pass                 @ masked
    LDR     r8, [r2, #spMode]
    MOV     r8, r8, ASR #27
    TEQ     r8, #6                  @ sprite type 6 = 32bpp
    BNE     sv_pass
    TEQ     r5, #0                  @ GCOL action: store only
    BNE     sv_pass
    LDR     r8, [r2, #spLBit]
    TEQ     r8, #0
    BNE     sv_pass                 @ left-hand wastage
    LDR     r8, [r2, #spRBit]
    TEQ     r8, #31
    BNE     sv_pass                 @ right-hand wastage
    TEQ     r6, #0
    BEQ     sv_scaled_ok
    LDMIA   r6, {r8, r9, r10, r11}
    TEQ     r8, r10
    TEQEQ   r9, r11
    BNE     sv_pass
sv_scaled_ok:
    ADR     r0, sv_vduvars
    ADR     r1, sv_vduvals
    SWI     XOS_ReadVduVariables
    BVS     sv_pass
    ADR     r1, sv_vduvals
    LDR     r8, [r1, #12]
    TEQ     r8, #5                  @ screen 32bpp too
    BNE     sv_pass

    @ OS units to pixels, and the bottom-left origin to the top-left one
    @ the device works in.
    LDR     r8, [r1, #0]            @ XEigFactor
    LDR     r9, [r1, #36]           @ OrgX
    ADD     r3, r3, r9
    MOV     r3, r3, ASR r8          @ left edge in pixels
    LDR     r8, [r1, #4]            @ YEigFactor
    LDR     r9, [r1, #40]           @ OrgY
    ADD     r4, r4, r9
    MOV     r4, r4, ASR r8          @ bottom edge, bottom origin

    LDR     r5, [r2, #spWidth]
    ADD     r5, r5, #1              @ pixels: one word each at 32bpp
    LDR     r6, [r2, #spHeight]
    ADD     r6, r6, #1
    LDR     r7, [r1, #16]
    ADD     r7, r7, #1              @ yres
    SUB     r9, r7, r4
    SUB     r9, r9, r6              @ top edge, top origin

    ADR     r10, sv_accoff
    LDR     r10, [r10]
    TEQ     r10, #0
    BNE     sv_pass                 @ *SprBench turns it off to compare

    @ Keep the biggest sprite seen, not the last: *SprBench replots it,
    @ and the last one is usually a 128x128 wallpaper tile whose plot
    @ rounds to nothing either way.
    MUL     r10, r6, r5
    ADR     r11, sv_lastarea
    LDR     r11, [r11]
    CMP     r10, r11
    BLS     .Lsv_nokeep
    ADR     r11, sv_lastarea
    STR     r10, [r11]
    ADR     r10, sv_lastspr
    STR     r2, [r10]
    STR     r1, [r10, #4]
.Lsv_nokeep:

    ADR     r10, blit_log
    LDR     r10, [r10]
    TEQ     r10, #0
    BEQ     sv_pass

    MOV     r11, #OP_SPRITE
    STR     r11, [r10, #BLIT_OP]
    MOV     r11, #F_SRC_VIRT        @ rows run top down, as the screen does
    STR     r11, [r10, #BLIT_FLAGS]
    LDR     r11, [r2, #spImage]
    ADD     r11, r11, r2            @ logical: the host walks the page tables
    STR     r11, [r10, #BLIT_SRC]
    MOV     r11, r5, LSL #2
    STR     r11, [r10, #BLIT_SSTRIDE]
    STR     r5, [r10, #BLIT_WIDTH]
    STR     r6, [r10, #BLIT_HEIGHT]
    STR     r3, [r10, #BLIT_DSTX]
    STR     r9, [r10, #BLIT_DSTY]
    MOV     r11, #4
    STR     r11, [r10, #BLIT_BPP]

    LDR     r11, [r1, #20]          @ GWLCol
    STR     r11, [r10, #BLIT_CLIPX0]
    LDR     r11, [r1, #28]          @ GWRCol
    STR     r11, [r10, #BLIT_CLIPX1]
    SUB     r8, r7, #1              @ yres - 1
    LDR     r11, [r1, #32]          @ GWTRow
    SUB     r11, r8, r11
    STR     r11, [r10, #BLIT_CLIPY0]
    LDR     r11, [r1, #24]          @ GWBRow
    SUB     r11, r8, r11
    STR     r11, [r10, #BLIT_CLIPY1]

    STR     r11, [r10, #BLIT_GO]
    LDR     r11, [r10, #BLIT_GO]
    TEQ     r11, #0
    BNE     sv_pass                 @ refused: leave it to SpriteExtend

    ADR     r8, sv_easyn
    LDR     r9, [r8]
    ADD     r9, r9, #1
    STR     r9, [r8]
    MUL     r9, r6, r5              @ pixels actually taken on
    ADR     r8, sv_accarea
    LDR     r10, [r8]
    ADD     r10, r10, r9
    STR     r10, [r8]

    @ Claim.  CallVector pushed the caller's return address before
    @ walking the chain, so passing on is MOV pc, lr and intercepting is
    @ taking that address off the stack once our own frame is gone.
    LDMFD   sp!, {r0-r11, lr}
    MSR     CPSR_f, #0              @ V clear: no error
    LDMFD   sp!, {pc}

sv_pass:
    ADR     r8, sv_passed
    LDR     r9, [r8]
    ADD     r9, r9, #1
    STR     r9, [r8]
    @ Area of what we turned away, so the split that matters -- pixels,
    @ not calls -- says whether the next case is worth writing.  Only
    @ range C, where r2 points at the sprite rather than naming it.
    LDR     r9, [sp]                @ r0 as it came in
    CMP     r9, #512
    BLO     sv_count
    LDR     r9, [r2, #spWidth]
    ADD     r9, r9, #1
    LDR     r10, [r2, #spHeight]
    ADD     r10, r10, #1
    MUL     r9, r10, r9
    ADR     r8, sv_passarea
    LDR     r10, [r8]
    ADD     r10, r10, r9
    STR     r10, [r8]

sv_count:
    LDR     r10, [sp]               @ r0 as it came in
    AND     r10, r10, #0xFF
    ADR     r6, sv_reasons
    MOV     r7, #0
sv_find:
    LDR     r8, [r6, r7, LSL #2]
    TEQ     r8, r10
    BEQ     sv_hit
    ADD     r7, r7, #1
    CMP     r7, #6
    BLO     sv_find
    B       sv_out
sv_hit:
    ADR     r6, sv_counts
    LDR     r8, [r6, r7, LSL #2]
    ADD     r8, r8, #1
    STR     r8, [r6, r7, LSL #2]
sv_out:
    LDMFD   sp!, {r0-r11, pc}

sv_vduvars:
    .word   4                       @ XEigFactor
    .word   5                       @ YEigFactor
    .word   6                       @ LineLength
    .word   9                       @ Log2BPP
    .word   12                      @ YWindLimit
    .word   0x80                    @ GWLCol
    .word   0x81                    @ GWBRow
    .word   0x82                    @ GWRCol
    .word   0x83                    @ GWTRow
    .word   0x88                    @ OrgX
    .word   0x89                    @ OrgY
    .word   -1
sv_vduvals:
    .space  4 * 11
sv_reasons:
    .word   28, 34, 48, 49, 50, 52
sv_maxarea:
    .word   0
sv_counts:
    .space  4 * 6
sv_n52:
    .word   0
sv_easyn:
    .word   0
sv_passed:
    .word   0
sv_accarea:
    .word   0
sv_passarea:
    .word   0
sv_accoff:
    .word   0
sv_lastspr:
    .word   0
    .word   0
sv_lastarea:
    .word   0
sv_hexbuf:
    .space  16
    .balign 4

@ ------------------------------------------------------------ *SprBench
@ Replot the last sprite the accelerator took on, forty times, with the
@ host doing it and then with SpriteExtend doing it.  A real sprite at
@ a real size, and the guest's own clock, because every attempt to time
@ this from outside was swamped by the pacing of the input driving it.
@ ------------------------------------------------------------ *SprBench
@ Plot a sprite this module owns, a thousand times, with the host doing
@ it and then with SpriteExtend doing it, timed by the guest's own
@ clock.  Owning the sprite is the point: the first attempt replotted
@ whichever sprite the desktop had last handed us, and NetSurf moved
@ its buffer out from under the pointer.
cmd_sprbench:
    STMFD   sp!, {r0-r9, lr}
    ADR     r6, sb_area
    LDR     r7, [r6]
    TEQ     r7, #0
    BNE     sb_ready

    MOV     r0, #ModClaim
    LDR     r3, =BENCH_AREA
    SWI     XOS_Module
    BVS     sb_out
    ADR     r6, sb_area
    STR     r2, [r6]

    LDR     r0, =BENCH_AREA
    STR     r0, [r2, #0]            @ saEnd
    MOV     r0, #1
    STR     r0, [r2, #4]            @ saNumber
    MOV     r0, #16
    STR     r0, [r2, #8]            @ saFirst
    LDR     r0, =BENCH_AREA
    STR     r0, [r2, #12]           @ saFree

    ADD     r1, r2, #16             @ the sprite itself
    LDR     r0, =44 + BENCH_IMG
    STR     r0, [r1, #0]            @ spNext
    LDR     r0, =0x636E6562         @ "benc"
    STR     r0, [r1, #4]
    MOV     r0, #0x68               @ "h"
    STR     r0, [r1, #8]
    MOV     r0, #0
    STR     r0, [r1, #12]
    MOV     r0, #BENCH_W - 1
    STR     r0, [r1, #16]           @ spWidth, words - 1
    MOV     r0, #BENCH_H - 1
    STR     r0, [r1, #20]           @ spHeight, rows - 1
    MOV     r0, #0
    STR     r0, [r1, #24]           @ spLBit
    MOV     r0, #31
    STR     r0, [r1, #28]           @ spRBit
    MOV     r0, #44
    STR     r0, [r1, #32]           @ spImage
    STR     r0, [r1, #36]           @ spTrans == spImage: unmasked
    LDR     r0, =0x301680B5         @ type 6, 32bpp, 90dpi
    STR     r0, [r1, #40]

sb_ready:
    MOV     r0, #26                 @ the Wimp leaves a graphics window
    SWI     XOS_WriteC              @ that would clip all of this away

    @ Pass one: the host.
    ADR     r6, sv_accoff
    MOV     r0, #0
    STR     r0, [r6]
    SWI     XOS_ReadMonotonicTime
    MOV     r9, r0
    LDR     r8, =BENCH_PLOTS
.Lsb_l1:
    ADR     r6, sb_area
    LDR     r1, [r6]
    ADD     r2, r1, #16
    LDR     r0, =512 + 52
    MOV     r3, #0
    MOV     r4, #0
    MOV     r5, #0
    MOV     r6, #0
    MOV     r7, #0
    SWI     XOS_SpriteOp
    SUBS    r8, r8, #1
    BNE     .Lsb_l1
    SWI     XOS_ReadMonotonicTime
    SUB     r0, r0, r9
    ADR     r6, sb_host
    STR     r0, [r6]

    @ Pass two: SpriteExtend, exactly as before any of this existed.
    ADR     r6, sv_accoff
    MOV     r0, #1
    STR     r0, [r6]
    SWI     XOS_ReadMonotonicTime
    MOV     r9, r0
    LDR     r8, =BENCH_PLOTS
.Lsb_l2:
    ADR     r6, sb_area
    LDR     r1, [r6]
    ADD     r2, r1, #16
    LDR     r0, =512 + 52
    MOV     r3, #0
    MOV     r4, #0
    MOV     r5, #0
    MOV     r6, #0
    MOV     r7, #0
    SWI     XOS_SpriteOp
    SUBS    r8, r8, #1
    BNE     .Lsb_l2
    SWI     XOS_ReadMonotonicTime
    SUB     r0, r0, r9
    ADR     r6, sb_guest
    STR     r0, [r6]

    ADR     r6, sv_accoff
    MOV     r0, #0
    STR     r0, [r6]

    ADR     r6, sb_host             @ centiseconds: host, then guest
    MOV     r8, #2
.Lsb_print:
    LDR     r0, [r6], #4
    ADR     r1, sv_hexbuf
    MOV     r2, #12
    SWI     XOS_ConvertHex8
    SWI     XOS_Write0
    SWI     XOS_WriteI + 32
    SUBS    r8, r8, #1
    BNE     .Lsb_print
    SWI     XOS_NewLine
sb_out:
    MSR     CPSR_f, #0
    LDMFD   sp!, {r0-r9, pc}

sb_area:
    .word   0
sb_host:
    .word   0
sb_guest:
    .word   0

cmd_sprstats:
    STMFD   sp!, {r0-r8, lr}
    ADR     r6, sv_maxarea
    MOV     r7, #12                 @ ..., accelerated, passed, and both areas
    MOV     r8, #0
sp_loop:
    LDR     r0, [r6], #4
    ADR     r1, sv_hexbuf
    MOV     r2, #12
    SWI     XOS_ConvertHex8
    BVS     sp_out
    SWI     XOS_Write0
    SWI     XOS_WriteI + 32
    ADD     r8, r8, #1
    TEQ     r8, #6
    MOVEQ   r8, #0
    SWIEQ   XOS_NewLine
    SUBS    r7, r7, #1
    BNE     sp_loop
    SWI     XOS_NewLine
sp_out:
    MSR     CPSR_f, #0
    LDMFD   sp!, {r0-r8, pc}

    .ltorg
