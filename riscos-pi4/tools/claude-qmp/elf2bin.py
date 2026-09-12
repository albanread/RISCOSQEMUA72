import struct, sys
data = open(sys.argv[1],'rb').read()
assert data[:4] == b'\x7fELF'
e_shoff, = struct.unpack_from('<I', data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)
def sh(i):
    o = e_shoff + i*e_shentsize
    return struct.unpack_from('<IIIIII', data, o)
stroff = sh(e_shstrndx)[4]
def nm(n):
    return data[stroff+n:data.index(b'\0', stroff+n)].decode()
for i in range(e_shnum):
    name, typ, flags, addr, off, size = sh(i)
    n = nm(name)
    if n.startswith('.rel'): sys.exit("relocations present: " + n)
    if n == '.text':
        open(sys.argv[2],'wb').write(data[off:off+size])
        print(f"{sys.argv[2]}: {size} bytes")
