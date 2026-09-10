@ hostfs_entries.s — the eight FSEntry veneers and the fsinfo-offset
@ helper (code only; the FSInfo block itself is hostfs_fsinfo.s, merged
@ into the generated module head).
@
@ The veneers follow the swi-handler shape from tools/gen_module.py: the
@ caller's R0-R9 are pushed as a block the C handler reads and writes,
@ the static base is fetched from the private word, and the handler's
@ return value becomes V (zero = clear, else R0 -> error block).

@ hostfs_fsinfo's own offset from the module base, as data: the C side
@ needs it for the OS_FSControl 12 registration and cannot read a
@ .module symbol's value under RWPI.  A difference of two symbols is
@ position-independent, so this is legal outside .module.
    .section .text,"ax",%progbits
    .align  2
    .global hostfs_fsinfo_offset
hostfs_fsinfo_offset:
    ldr     r0, 1f
1:  .word   hostfs_fsinfo - 1b
    bx      lr

@ ----------------------------------------------------------------------------
@ The eight FSEntry veneers.  In: SVC mode, R12 -> private word, may
@ trash R0-R12, must return through LR; errors: V set, R0 -> error block.
@ ----------------------------------------------------------------------------

.macro FSENTRY name, csym
    .section .text,"ax",%progbits
    .align  2
    .global hostfs_fs_\name
    .type   hostfs_fs_\name, %function
hostfs_fs_\name:
    stmfd   sp!, {r0-r9, lr}        @ the caller's registers, as a block
    mov     r0, sp                  @ -> that block
    ldr     r9, [r12]               @ static base from the private word
    ldr     r1, [r9, #-8]           @ the module workspace pointer
    bl      \csym
    cmp     r0, #0
    bne     91f
    ldmfd   sp!, {r0-r9, lr}        @ hand back whatever the handler wrote
    msr     cpsr_f, #0
    mov     pc, lr
91: mov     r11, r0                 @ park the error pointer out of the
    ldmfd   sp!, {r0-r9, lr}        @   unwind's reach
    mov     r0, r11
    msr     cpsr_f, #(1 << 28)
    mov     pc, lr
.endm

FSENTRY open,  hostfs_c_open
FSENTRY get,   hostfs_c_get
FSENTRY put,   hostfs_c_put
FSENTRY args,  hostfs_c_args
FSENTRY close, hostfs_c_close
FSENTRY file,  hostfs_c_file
FSENTRY func,  hostfs_c_func
