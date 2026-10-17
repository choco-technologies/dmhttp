#!/usr/bin/env python3
"""Reject absolute pointers outside legacy DMOD relocation sections.

Read-only ELF audit: it never modifies ELF/DMF or emits runtime relocation data.
The fork must use DMOD_GLOBAL_POINTER or eliminate stored absolute pointers.
"""
import struct
import sys
from pathlib import Path


def check(elf, dmf):
    if len(elf)<64 or elf[:4]!=b'\x7fELF' or elf[5]!=1:
        raise ValueError('expected little-endian ELF')
    wide=elf[4]==2
    machine=struct.unpack_from('<H',elf,18)[0]
    if (machine,wide) not in ((40,False),(62,True)):
        raise ValueError('pointer audit currently supports ARM ELF32 and x86-64 ELF64')
    width=8 if wide else 4
    if len(dmf)<240 or struct.unpack_from('<I',dmf)[0]!=0x444d4f44 or struct.unpack_from('<I',dmf,4)[0]!=240 or struct.unpack_from('<I',dmf,12)[0]!=width:
        raise ValueError('expected the unchanged 240-byte module header')
    shoff=struct.unpack_from('<Q' if wide else '<I',elf,40 if wide else 32)[0]
    stride,count,names_index=struct.unpack_from('<HHH',elf,58 if wide else 46)
    fmt='<IIQQQQIIQQ' if wide else '<10I'
    if stride!=struct.calcsize(fmt) or names_index>=count:
        raise ValueError('invalid ELF section table')
    sections=[struct.unpack_from(fmt,elf,shoff+i*stride) for i in range(count)]
    ns=sections[names_index];names=elf[ns[4]:ns[4]+ns[5]]
    def name(section):
        start=section[0];return names[start:names.index(b'\0',start)].decode('ascii')
    allowed={'.header','.inputs','.outputs','.got'}
    absolute=(1,10,11) if wide else (2,5,6,8,38,43,44,47,48)
    relative=(0,2,4,9,24,25,26,27,28,29,41,42) if wide else (0,3,10,24,25,26,28,29,30,42,51,96,102,103)
    records=0;got=0;failures=[]
    for section in sections:
        if section[1]!=(4 if wide else 9):continue
        target=sections[section[7]]
        if not target[2]&2:continue
        symbol_table=sections[section[6]]
        expected=24 if wide else 8
        if section[9]!=expected:raise ValueError('invalid relocation record size')
        for pos in range(section[4],section[4]+section[5],expected):
            offset,info=struct.unpack_from('<QQ' if wide else '<II',elf,pos)
            kind,symbol=(info&0xffffffff,info>>32) if wide else (info&255,info>>8)
            records+=1
            if kind in absolute:
                symbol_index=struct.unpack_from('<H',elf,symbol_table[4]+symbol*(24 if wide else 16)+(6 if wide else 14))[0]
                if symbol_index==0xfff1:continue # SHN_ABS: a link-time constant
                section_name=name(target)
                if section_name not in allowed:
                    failures.append(f'{section_name}+{offset-target[3]:#x}: absolute relocation {kind}')
                if section_name=='.got':got+=1
            elif kind not in relative:
                failures.append(f'{name(target)}: unsupported relocation {kind}')
    if records==0:raise ValueError('missing ELF relocation metadata; link with --emit-relocs')
    if failures:raise ValueError('pointers outside legacy DMOD relocation sections:\n'+'\n'.join(failures))
    return got


def main():
    try:
        got=check(Path(sys.argv[1]).read_bytes(),Path(sys.argv[2]).read_bytes())
    except (ValueError,IndexError,struct.error) as error:
        raise SystemExit(str(error))
    print(f'DMOD pointer audit passed: legacy header, {got} GOT relocations, no extra fixups')

if __name__=='__main__':main()
