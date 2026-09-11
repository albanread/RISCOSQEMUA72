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
    .byte   0                       @ end of table
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
    MSR     CPSR_f, #0
    MOV     pc, lr

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
