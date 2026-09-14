@ HostNet: the two veneers that are not SWI entries.
@
@ A RISC OS program learns that a socket has something on it from Internet
@ Event 19, which the Internet module used to raise from its own receive
@ path.  There is no receive path here -- the host has the socket -- and
@ the host cannot call into the guest, so the module has to ask.  It asks
@ on a ticker.
@
@ The ticker itself does almost nothing.  Ticker routines run in IRQ mode
@ with interrupts off and very little of the OS available; raising events
@ and ringing the doorbell from there would be asking for trouble.  So the
@ ticker adds a callback, and the callback -- which runs in USR mode with
@ the machine in a normal state -- does the work.  That is the same shape
@ the Internet module uses for its own deferred work (setsoftnet ->
@ OS_AddCallBack -> callb_handler in riscos/c/setsoft).
@
@ Both are entered with R12 holding whatever was passed as the R12 value to
@ OS_CallEvery / OS_AddCallBack.  The module passes its *static base* --
@ which, built -frwpi, is exactly what R9 must hold for the C code to find
@ its variables -- so the prologue is a move rather than the load through a
@ private word that the generated SWI veneers do.

    .section .text,"ax",%progbits
    .align  2

    .global hostnet_tick
    .type   hostnet_tick, %function
hostnet_tick:
    stmfd   sp!, {r0-r3, r9, lr}
    mov     r9, r12                 @ static base, as passed to OS_CallEvery
    bl      hostnet_c_tick
    ldmfd   sp!, {r0-r3, r9, lr}
    mov     pc, lr

    .global hostnet_callback
    .type   hostnet_callback, %function
hostnet_callback:
    stmfd   sp!, {r0-r9, lr}
    mov     r9, r12                 @ ditto, from OS_AddCallBack
    bl      hostnet_c_callback
    ldmfd   sp!, {r0-r9, lr}
    mov     pc, lr
