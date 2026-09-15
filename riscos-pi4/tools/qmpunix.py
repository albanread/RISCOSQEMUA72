#!/usr/bin/env python3
"""A QMP client transport over a unix socket, on any host Python.

Why a unix socket and not loopback TCP: QMP has no authentication, and a
loopback port is reachable from the guest.  With HostNet the guest's
sockets are the host's sockets, so the port is one Socket_Connect away
for any program the guest runs; and even without it, slirp's 10.0.2.2
alias forwards to the host's loopback.  A unix socket is a file the
guest has no way to open (ROS_PRIVATE#34).

macOS and Linux Python speak AF_UNIX natively.  Windows Python gained
AF_UNIX only in 3.13, so on older Windows Pythons the connection is made
through ws2_32 directly -- WSASocketW, connect to a sockaddr_un -- and
the SOCKET is wrapped with _open_osfhandle into an ordinary binary file.
QEMU's Windows build has bound AF_UNIX chardevs since 8.2, so both ends
work on every host this tree builds on.

settimeout() is honoured on the native path only; through the shim it is
accepted and ignored, so a call on a wedged emulator blocks until EOF
rather than timing out mid-conversation.
"""

import ctypes
import os
import socket

if os.name == "nt":
    import msvcrt

    # ws2_32 calls that take a SOCKET: the handle is pointer-sized, so
    # the conversions have to be stated or ctypes truncates to c_int.
    _ws2 = ctypes.WinDLL("ws2_32", use_last_error=True)
    _ws2.WSASocketW.restype = ctypes.c_size_t
    _ws2.WSASocketW.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                ctypes.c_void_p, ctypes.c_void_p,
                                ctypes.c_ulong]
    _ws2.connect.restype = ctypes.c_int
    _ws2.connect.argtypes = [ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    _ws2.closesocket.restype = ctypes.c_int
    _ws2.closesocket.argtypes = [ctypes.c_size_t]

    _AF_UNIX = 1                      # winsock2.h: AF_UNIX
    _INVALID_SOCKET = ~ctypes.c_size_t(0).value


class QmpFile:
    """The half of a socket the QMP helpers need: readline, write, close."""

    def __init__(self, stream, sock=None):
        self._stream = stream
        self._sock = sock             # kept for settimeout and close

    def readline(self):
        return self._stream.readline()

    def write(self, data):
        self._stream.write(data)

    def settimeout(self, seconds):
        if self._sock is not None:
            self._sock.settimeout(seconds)

    def close(self):
        try:
            self._stream.close()
        finally:
            if self._sock is not None:
                self._sock.close()


def connect(path, timeout=None):
    """Open the QMP unix socket at `path`; returns a QmpFile."""
    path = os.fspath(path)

    if hasattr(socket, "AF_UNIX"):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            if timeout is not None:
                s.settimeout(timeout)
            s.connect(path)
            stream = s.makefile("rwb", buffering=0)
        except OSError:
            s.close()
            raise
        return QmpFile(stream, s)

    if os.name != "nt":
        raise OSError(f"no AF_UNIX in this Python and no shim on {os.name}")

    sock = _ws2.WSASocketW(_AF_UNIX, socket.SOCK_STREAM, 0, None, None, 0)
    if sock == _INVALID_SOCKET:
        raise OSError(ctypes.get_last_error(), "WSASocketW(AF_UNIX)")

    name = os.fsencode(path)
    if len(name) >= 108:
        _ws2.closesocket(sock)
        raise ValueError(f"path too long for sockaddr_un: {path}")
    sa = ctypes.create_string_buffer(2 + len(name) + 1)
    ctypes.memmove(sa, _AF_UNIX.to_bytes(2, "little"), 2)
    ctypes.memmove(ctypes.byref(sa, 2), name, len(name))

    if _ws2.connect(sock, sa, len(sa)) != 0:
        err = ctypes.get_last_error()
        _ws2.closesocket(sock)
        raise OSError(err, f"connect({path})")

    fd = msvcrt.open_osfhandle(sock, 0)
    if fd == -1:
        _ws2.closesocket(sock)
        raise OSError("open_osfhandle")
    return QmpFile(os.fdopen(fd, "r+b", buffering=0))
