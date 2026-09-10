    .arch armv7-a
    .text
    .global _start
_start:
    mrc  p15, 0, r0, c0, c0, 5   @ MPIDR: only core 0 runs the test,
    ands r0, r0, #3              @ so the output is one line, not four
    bne  .                       @ secondaries park here
    movw r9, #:lower16:0xFE201000
    movt r9, #:upper16:0xFE201000
    movw r8, #:lower16:0xFE003004
    movt r8, #:upper16:0xFE003004
    mov  r0, #0x301
    str  r0, [r9, #0x30]        @ UARTCR = UARTEN|TXE|RXE

    @ program the framebuffer over mailbox channel 1, so the window's
    @ full pipeline (upload+decode+scale+present at vsync) runs while
    @ the loop below is timed
    movw r3, #:lower16:0x1000
    movt r3, #:upper16:0x1000
    mov  r0, #800
    str  r0, [r3, #0]
    mov  r0, #600
    str  r0, [r3, #4]
    mov  r0, #800
    str  r0, [r3, #8]
    mov  r0, #600
    str  r0, [r3, #12]
    mov  r0, #0
    str  r0, [r3, #16]
    mov  r0, #32
    str  r0, [r3, #20]
    mov  r0, #0
    str  r0, [r3, #24]
    str  r0, [r3, #28]
    str  r0, [r3, #32]
    str  r0, [r3, #36]
    movw r0, #:lower16:0xFE00B8A0
    movt r0, #:upper16:0xFE00B8A0
    mov  r1, #0x1001
    str  r1, [r0]
    @ settle: let the pipeline build and a few frames present
    movw r1, #:lower16:0x02000000
    movt r1, #:upper16:0x02000000
9:  subs r1, r1, #1
    bne  9b
    ldr  r4, [r8]               @ t0 (1MHz free-running)
    movw r0, #:lower16:100000000
    movt r0, #:upper16:100000000
1:  subs r0, r0, #1
    bne  1b
    ldr  r5, [r8]
    sub  r5, r5, r4             @ elapsed microseconds
    mov  r6, #28
2:  lsr  r0, r5, r6
    and  r0, r0, #0xf
    cmp  r0, #10
    addlt r0, r0, #48
    addge r0, r0, #87
    str  r0, [r9]
    subs r6, r6, #4
    bge  2b
    mov  r0, #10
    str  r0, [r9]
3:  b 3b
