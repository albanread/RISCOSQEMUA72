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
    .byte   0                       @ end of table
    .balign 4
st_syntax:
    .asciz  "Syntax:\t*BlitStats"
st_help:
    .asciz  "*BlitStats reports the FillRectangle calls seen on GraphicsV.\r"
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

final:
    STMFD   sp!, {r0-r2, lr}
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
    .ltorg
