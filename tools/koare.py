"""Tiny RE helpers over koa.exe (RVA-based; VA = 0x400000 + RVA).
Set KOA_EXE to the path of koa.exe. Usage: python koare.py <rva-hex> [marked rvas...] -> disassembles the function."""
import pefile, capstone, sys, struct
import os
PE = pefile.PE(os.environ.get('KOA_EXE', 'koa.exe'), fast_load=True)
IMG = PE.get_memory_mapped_image()
BASE = 0x400000
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = False

def func_start(rva, maxback=0x4000):
    """walk backwards to the nearest int3/nop padding boundary followed by code"""
    p = rva
    while p > rva - maxback:
        if IMG[p-1] in (0xCC, 0x90) and IMG[p-2] in (0xCC, 0x90) and IMG[p] not in (0xCC, 0x90):
            # require 16-byte alignment typical of MSVC
            if p % 16 == 0: return p
        p -= 1
    return None

def func_end(start, maxlen=0x8000):
    p = start
    for i in md.disasm(IMG[start:start+maxlen], BASE+start):
        r = i.address - BASE
        if i.mnemonic == 'int3' and IMG[r+1] == 0xCC: return r
        p = r
    return p

def dis(start, end, mark=()):
    out = []
    for i in md.disasm(IMG[start:end], BASE+start):
        r = i.address - BASE
        tag = '>>' if r in mark else '  '
        op = i.op_str
        out.append(f'{tag}{r:06X}: {i.mnemonic} {op}')
    return out

def calls_in(start, end):
    res = []
    for i in md.disasm(IMG[start:end], BASE+start):
        if i.mnemonic == 'call':
            res.append((i.address-BASE, i.op_str))
    return res

def rtti_name_of_vtable(vt_rva):
    """vt_rva: rva of vtable; COL pointer at vt-4"""
    col = struct.unpack_from('<I', IMG, vt_rva-4)[0] - BASE
    td = struct.unpack_from('<I', IMG, col+12)[0] - BASE
    name = IMG[td+8:td+200].split(b'\0')[0]
    return name.decode(errors='replace')

if __name__ == '__main__':
    rva = int(sys.argv[1], 16)
    s = func_start(rva); e = func_end(s)
    print(f'func {s:06X}-{e:06X} size {e-s}')
    for l in dis(s, e+1, mark=[int(x,16) for x in sys.argv[2:]]): print(l)
