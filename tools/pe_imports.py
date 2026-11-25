"""Read-only PE import inventory; reads are bounded by headers and raw sections."""
import struct
from pathlib import Path


def inventory(path):
    with Path(path).open('rb') as stream:
        stream.seek(0, 2)
        size = stream.tell()
        return _inventory(stream, size)


def _inventory(stream, file_size):
    def span(offset, size):
        if offset < 0 or size < 0 or offset > file_size - size:
            raise ValueError('Truncated PE')

    def block(offset, size):
        span(offset, size)
        stream.seek(offset)
        data = stream.read(size)
        if len(data) != size:
            raise ValueError('Truncated PE')
        return data

    def read(fmt,offset):
        return struct.unpack(fmt, block(offset, struct.calcsize(fmt)))

    if block(0, 2) != b'MZ': raise ValueError('Not PE')
    pe=read('<I',60)[0]
    if block(pe, 4) != b'PE\0\0': raise ValueError('Not PE')
    span(pe, 24)
    machine,count=read('<HH',pe+4)
    optional=pe+24
    length=read('<H',pe+20)[0]
    if length < 112: raise ValueError('Truncated optional header')
    span(optional, length)
    if machine!=0x8664 or read('<H',optional)[0]!=0x20b: raise ValueError('Expected x64 PE32+')
    directory_count = read('<I', optional + 108)[0]
    if directory_count > (length - 112) // 8:
        raise ValueError('Invalid PE directory count')
    header_size = read('<I', optional + 60)[0]
    span(0, header_size)
    if header_size < optional + length + count * 40:
        raise ValueError('Section table outside PE headers')
    sections=[]
    for i in range(count):
        offset=optional+length+i*40
        virtual_size,address,raw_size,raw=read('<IIII',offset+8)
        span(raw, raw_size)
        sections.append((address,max(virtual_size,raw_size),raw,raw_size))

    def rva(value):
        if value < header_size:
            return value, header_size - value
        for address,size,raw,raw_size in sections:
            if address <= value < address + size and value - address < raw_size:
                available = raw_size - (value - address)
                return raw + value - address, available
        raise ValueError('Invalid PE RVA')

    def name(value):
        offset, available = rva(value)
        data = block(offset, min(256, available))
        end = data.find(b'\0')
        if end<0: raise ValueError('Invalid import name')
        text=data[:end].decode('ascii').lower()
        if not text.endswith('.dll') or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_.-' for c in text): raise ValueError('Unexpected import')
        return text

    def directory(index):
        if index >= directory_count:
            return 0, 0
        address, size = read('<II', optional + 112 + index * 8)
        if bool(address) != bool(size):
            raise ValueError('Incomplete PE directory')
        return address, size

    result=set()
    for index,stride,name_offset in ((1,20,12),(13,32,4)):
        address,size=directory(index)
        if not address: continue
        offset, available = rva(address)
        if size > available: raise ValueError('Import directory outside raw section')
        for i in range(min(size//stride,4096)):
            entry = block(offset + i * stride, stride)
            if not any(entry): break
            if index==13 and struct.unpack_from('<I', entry)[0]!=1: raise ValueError('Unsupported legacy delay import')
            result.add(name(struct.unpack_from('<I', entry, name_offset)[0]))
        else: raise ValueError('Unterminated imports')
    cert_offset,cert_size=directory(4)
    span(cert_offset, cert_size)
    return {'machine':'x64','imports':sorted(result),'signature':'UNSIGNED' if not cert_offset and not cert_size else 'CERTIFICATE PRESENT, NOT VERIFIED'}
