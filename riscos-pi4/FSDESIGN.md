# FSDESIGN — `HostFS:` and the vmchannel device

How files move between the Windows host and the guest without touching
the ROM, the network, or the card image.  This is route 2 of Sprint 6;
route 1 (LanManFS over slirp) is dead for a structural reason recorded
in `SPRINTS.md` — the ROM's SMB client speaks only SMB1 and this
Windows runs SMB2-only with signing required.  The precedent is good:
VirtualAcorn and RPCEmu both ship a `HostFS:` whose module talks to the
emulator; the difference here is the transport, which is a device
rather than intercepted SWIs, so no CPU state is forged and any guest
code could use it.

## Shape

    RISC OS 5                        QEMU (Windows host)
    ----------                       -------------------
    HostFS module                    vmchannel device (MMIO)
      FSEntry_* vectors      --->      doorbell register write
      builds request block             handler runs the command
      polls STATUS register            against root=<host dir>
      reads response block         <--- writes response into the block

Nothing here is asynchronous, queued, or interrupt-driven.  The guest
writes a request block into its own RAM, pokes the doorbell with its
physical address, and polls a status register.  QEMU executes the
command synchronously inside the MMIO write, under the BQL — the same
way the framebuffer mailbox tags are handled.  A slow host disc stalls
the vCPU for the duration, which is exactly what a slow disc does on
real hardware; in exchange there is no ring, no interrupts, no reorder
window, nothing to migrate beyond four registers, and no way for guest
and host views of a request to disagree.

## The device

A 16 KiB sysbus MMIO region at a physical address that is *unnamed by
the HAL and unmapped by QEMU* — RISC OS on the Pi reads no device tree,
so a hole the HAL does not name does not exist as far as it is
concerned.  Candidate at implementation time: the unimplemented stretch
around `0xfd400000` (the GENET stub at `0xfd580000` set the precedent
for reaching into that window).  The survey is the first task of the
device sprint: check `qemu-hw-arms-raspi4b`'s map and the BCM2835 HAL's
device table, then fix the address here.

Registers (32-bit, little-endian):

    0x00  MAGIC    read: 'VMCH' — identify, so the module can probe safely
    0x04  VERSION  read: 0 for the protocol in this document
    0x08  FEATURES read: bitmask, 1 = file ops, 2 = console, 4 = time
    0x0c  CMD      write: guest-physical address of a request block
                   (16-byte aligned; this write is the doorbell)
    0x10  STATUS   read: 0 = busy/no request, 1 = done, result in block
                   (the handler is synchronous, so STATUS will in
                   practice already read 1 when the write returns)
    0x14  RESERVED, reads as 0

The handler clamps every host path it builds to the configured root by
canonicalising and re-checking the prefix; `..` and absolute host paths
in guest-supplied names are refused.  The root is a device property:

    -device vmchannel,root=F:/RISCOSDEV/pi-share

Snapshotting is free: the registers hold no pointers between requests,
there is no queue, and a request in flight at `savevm` time is atomic
with the doorbell write that delivers it.

## The request block

Guest RAM, physical addressing (the device translates through its DMA
address space exactly as the framebuffer does), little-endian, 64-byte
fixed header then command-specific fields, then inline data:

    +0   u32  cmd
    +4   u32  seq       (copied to the response; the module may check it)
    +8   u32  rc        (response: 0 = OK, else a host errno-ish code)
    +12  u32  handle    (open file handle, or scratch)
    +16  u32  arg_len   (bytes of inline data following the header)
    +20   ..   command-specific words, zero-padded to +64
    +64   ..   arg_len bytes: path text, file data, catalogue entries

Commands, version 0:

    0  PING          arg: 4 bytes; response echoes them.  The smoke test.
    1  OPEN          arg: path; +16: RISC OS open flags.  Response:
                     handle, or rc = not-found / access.
    2  CLOSE         handle.  No response data.
    3  READ          handle, +16: guest-physical address of destination,
                     +20: length.  Response: bytes actually read.
    4  WRITE         handle, +16: guest-physical address of source,
                     +20: length.  Response: bytes written.
    5  SEEK          handle, +16: offset, +20: whence.  Response: offset.
    6  FILEARGS      path.  Response: RISC OS file metadata — load/exec
                     addresses derived from the host mtime, size, type,
                     attributes — the shape `OS_File 17` expects, so the
                     Filer and *Cat show honest information.
    7  CAT           path (a directory).  Response: array of entries,
                     each 48 bytes: name[40], type, size, attributes.
                     One level, like *Cat.
    8  CREATE        path, +16: type, +20: size.  (Files; directories by
                     creating a file inside them.)
    9  DELETE        path.
    10 RENAME        arg: two paths, newline-separated.
    16 CONSOLE       arg: bytes.  Appended to the host log (FEATURES bit 1).
    17 TIME          response: host UTC.  The module sets the guest clock
                     with OS_Word 14,1 at boot and on demand.

Path translation: the module sends RISC OS paths rooted at `$` (e.g.
`$.dev.main.s`); the device maps `.` to the host separator and the `$`
to the root.  Case sensitivity in v0 is the host's own (exact); if that
hurts in practice, v1 adds a case-insensitive directory scan in the
device, decided after the module works.

## The module

Built with `roscc` on the pattern of `compiler/tests/module/mojomod` —
a relocatable module (`&FFA`), C-ABI body, generated veneers, writable
statics claimed from the RMA, position independence checked at link
time.  It ships as a file on the card image, soft-loaded from
`!Boot.Choices.Boot.PreDesk`, so the ROM stays pristine and the
developer loop is "drop in a new build and reboot".

It registers itself as a filing system — name `HostFS`, an unclaimed
FS number chosen at implementation time — and implements the FSEntry
points the desktop actually exercises: Open/GetBytes/PutBytes/Close/
Args/File/Cat, plus `*HostFSPing` (raw doorbell test) and `*Bye`-safe
finalisation.  `HostFS::F.$.dev.main.s` is then a real path: the icon
bar gains a `HostFS` disc, `*Cat HostFS:$` lists the root, and the
Filer opens directories by CAT.  Load/exec addresses are synthesised
from the host mtime (dated file, type from the `,lxa` suffix if
present, else &FFF) — the same convention RISC OS uses for DOS discs.

Console capture is the module's cheapest extra: a `*HostConsole on`
that claims the write vectors and forwards output as CONSOLE commands,
so `*Cat` output and compiler chatter land in a host-side log file —
the second half of Sprint 6's acceptance, riding the same doorbell.

## Testing ladder

1. **Bare-metal smoke** (no RISC OS): a bench-harness guest pings the
   doorbell and prints the echoed bytes over the PL011 — the device,
   the address, and the protocol survive contact before any module
   exists.
2. **`*HostFSPing`** from the F12 CLI once the module loads.
3. **Read**: `*Cat HostFS:$` lists the files seeded in the share
   directory; double-click one from the Filer.
4. **Write**: create a file from RISC OS (e.g. save from an editor or
   `*Copy`), confirm it appears on the host with sane metadata.
5. **Snapshots**: `savevm` with HostFS mounted, `loadvm`, files still
   there — the no-pointers device makes this true by construction, but
   it gets tested anyway.

## Acceptance

From `SPRINTS.md`, unchanged: a file written on the host is readable
from the RISC OS desktop within a second and without a reboot, and a
`*Cat` typed in the guest appears on the host.
