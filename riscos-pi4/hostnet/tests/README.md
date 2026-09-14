# HostNet test programs

BBC BASIC, because it is the only language in the ROM and these have to
run on a machine with no toolchain on it.  Plain text: copy one into the
share and run it with

    BASIC -quit HostFS::HostFS.$.<name>

Each writes what it did to a file in the share, so the result can be read
from the host without watching the screen.

| program | what it proves |
| --- | --- |
| `listen.bas` | A guest *server*: bind, listen, accept a connection from the host, read it with `Recvmsg_1` and reply with `Sendmsg_1`. Exercises the listener path, the blocking-accept wait loop, and msghdr/iovec marshalling. Writes `$.listenlog`. |
| `evtest.bas` | A guest *client* that never polls and never reads on its own: it connects, asks for `FIOASYNC`, and then sits in an empty loop. Everything after that happens because Internet Event 19 arrived. Writes `$.evlog`. |

`listen.bas` binds port 9000 and the host connects to `127.0.0.1:9000` —
the guest's sockets *are* host sockets, so a guest listener is reachable
from the host with nothing in between.

`evtest.bas` needs a host-side peer that accepts, sends a line, waits, and
closes: that sequence produces an `async` event and then a `broken` one,
which is what it is there to check.  Run the emulator with
`HOSTNET_TRACE=<file>` and the poll lines name the reason:

    hostnet: POLL #577 -> 1 ready fd1=async:64641
    hostnet: POLL #743 -> 1 ready fd1=broken:64641
