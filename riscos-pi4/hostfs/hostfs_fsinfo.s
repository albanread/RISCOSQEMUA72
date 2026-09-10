@ hostfs_fsinfo.s — the FSInfo block, merged into the generated module
@ head (build-hostfs.sh concatenates the two) so it lands in the one
@ .module section roscc keeps.
@
@ This is what OS_FSControl 12 (FSControl_AddFS) receives as R2: every
@ word is an offset from the module base, and roscc links the module at
@ base 0, so link-time symbol values ARE those offsets.  Absolute words
@ are legal in .module and nowhere else.

    .section .module,"a",%progbits
    .align  2

    .global hostfs_fsinfo
hostfs_fsinfo:
    .word   hostfs_name                 @ FS_name
    .word   0                           @ FS_startuptext: none
    .word   hostfs_fs_open              @ FS_open
    .word   hostfs_fs_get               @ FS_get
    .word   hostfs_fs_put               @ FS_put
    .word   hostfs_fs_args              @ FS_args
    .word   hostfs_fs_close             @ FS_close
    .word   hostfs_fs_file              @ FS_file
    .word   220                         @ FS_info: fsnumber 220, no flags
    .word   hostfs_fs_func              @ FS_func
    .word   0                           @ FS_gbpb: let FileSwitch
                                        @   synthesise GBPB from get/put

hostfs_name:
    .asciz  "HostFS"
