# FATSTICK — a host directory as the guest's USB stick

The idea: QEMU's `vvfat` block driver exposes a host directory to the
guest as a FAT-formatted USB mass-storage stick.  RISC OS mounts USB
sticks through USBDriver + SCSIFS + DOSFS, so a directory of files
would appear as a disc — a second host-transfer route that needs no
guest module at all, and the natural way to deliver a new `HostFS,ffa`
without touching the card image.  This note records what the Mac
experiments established; the transfer route that actually ships today
is HostFS over the vmchannel doorbell (FSDESIGN.md).

## How to attach it

    -drive file=fat:rw:<hostdir>,format=raw,if=none,id=ud0 \
    -device usb-storage,drive=ud0,bus=usb-bus.0,port=1.4

Both launchers pass extra arguments straight through, so this rides
`run-app.sh`/`run-macos.sh` without changes.  Facts from the source
(`block/vvfat.c`) and the wire:

- Disk mode (a directory, not the floppy form): a real MBR with one
  bootable LBA partition starting at sector 0x3f, type 0x0e/0x0c
  (FAT16/32 LBA), volume label `QEMU VVFAT` — exactly the shape a real
  stick has.
- `rw` writes land in the host directory through a qcow overlay; guest
  deletes/renames are the driver's known weak spots.
- RISC OS file types ride FAT as `,xxx` hex suffixes — the same
  convention the vmchannel device synthesises, so nothing new to learn.

## What the guest did with it (so far)

Trace method, worth keeping for any USB work: launch with
`-trace enable=usb_msd_*,file=/tmp/usbtrace.log` and every SCSI
command the guest issues is on the host in plain text.

The ROM-only boot (5.30, `SCSIDriver` + `SCSIFS` + `DOSFS` +
`USBDriver` + `DWCDriver` all present) **does** claim the device far
enough to run the bulk-only transport: three bus resets, a max-LUN
probe, and one INQUIRY per reset (`len 6, data-len 36`).  Then it
stops — no TEST UNIT READY, no READ CAPACITY, no sector reads, whether
`removable=on` is set or not.  Whatever rejects the stick is parsing
the INQUIRY reply itself (vendor/product/version/qualifier — QEMU's
`scsi-hd` answers as a modern SPC device) or is a softloaded class
driver the ROM-only boot does not have.

So on this boot the stick is visible on the bus (`info usb` shows
`QEMU USB MSD`) but never mounts, and files placed on it from the host
are unreachable from the guest.

## Next steps, in order of cheapness

1. Retry against the working card image (its `!Boot` may softload a
   newer SCSIFS/SCSI glue than the ROM carries).  If it mounts, the
   route is done: `*RMLoad <stick>:HostFS` becomes the module-delivery
   path.
2. Diff QEMU's 36 INQUIRY bytes against what the RISC OS driver
   accepts (the ROOL sources); shape the reply if a field is the gate.
3. If neither bites, drop the route: HostFS over vmchannel already
   moves files both ways, and a stick is only nicer for the first
   bootstrap.
