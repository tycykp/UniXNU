"""Build a position-independent AArch64 NLH2 module on Linux/WSL.

The ELF is a build artifact only. The kernel receives a checked header plus
RX code, and resolves no symbols from the kernel or another module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from nlh_format import CORE_MODULES, MAX_CODE, KINDS, pack_image, parse_image

ROOT = Path(__file__).resolve().parent


def check_object(data):
    """Reject absolute relocations before the static linker can hide them.

    Allow only intra-image PC-relative branches/addresses and their LO12 parts.
    This checks compiler output, not the behavior of arbitrary native code.
    """
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (1, 183):
        raise ValueError('Expected relocatable AArch64 ELF64 object')
    shoff, = struct.unpack_from('<Q', data, 40)
    shsize, shnum = struct.unpack_from('<HH', data, 58)
    if shsize != 64 or not shnum or shoff + shsize * shnum > len(data):
        raise ValueError('Invalid object section table')
    allowed = {260, 261, 262, 273, 274, 275, 276, 277, 278, 279, 280,
               282, 283, 284, 285, 286, 299}
    for i in range(shnum):
        _, typ, _, _, off, size, _, _, _, stride = struct.unpack_from('<IIQQQQIIQQ', data, shoff + 64 * i)
        if typ == 9 and size: raise ValueError('Unexpected REL relocation table')
        if typ != 4 or not size: continue
        if stride != 24 or size % stride or off + size > len(data):
            raise ValueError('Invalid object relocation table')
        for at in range(off, off + size, stride):
            _, info, _ = struct.unpack_from('<QQq', data, at)
            if info & 0xffffffff not in allowed:
                raise ValueError(f'Non-position-independent relocation: {info & 0xffffffff}')


def extract_elf(data):
    if len(data) < 64 or data[:7] != b'\x7fELF\x02\x01\x01':
        raise ValueError('Expected little-endian ELF64')
    kind, machine, version, entry, phoff, shoff, flags, ehsize, phsize, phnum, shsize, shnum, shstr = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    if (kind, machine, version, ehsize, phsize, shsize) != (2, 183, 1, 64, 56, 64):
        raise ValueError('Expected linked AArch64 executable')

    def records(offset, size, count):
        if not count or offset > len(data) or count > (len(data) - offset) // size:
            raise ValueError('ELF table outside file')
        return [data[offset+i*size:offset+(i+1)*size] for i in range(count)]

    segments = [struct.unpack('<IIQQQQQQ', p) for p in records(phoff, phsize, phnum)]
    loads = [p for p in segments if p[0] == 1]
    if len(loads) != 1 or any(p[0] in (2, 3, 7) for p in segments):
        raise ValueError('One RX load segment required; no dynamic loader or TLS')
    _, prot, offset, address, _, size, memory, align = loads[0]
    if prot != 5 or address or size != memory or not 4 <= size <= MAX_CODE or offset + size > len(data):
        raise ValueError('Expected a zero-based RX image without BSS')
    if entry & 3 or entry > size - 4:
        raise ValueError('Entry must be an aligned instruction inside code')
    sections = [struct.unpack('<IIQQQQIIQQ', s) for s in records(shoff, shsize, shnum)]
    entry_found = False
    state_bytes = None
    for _, typ, attrs, addr, off, length, link, info, alignment, entsize in sections:
        if typ in (4, 9) and length:
            raise ValueError('Unresolved relocations')
        if attrs & 2 and (attrs & (1 | 0x400) or addr + length > size or (typ == 8 and length)):
            raise ValueError('Writable, TLS or out-of-range allocated section')
        if typ != 8 and off + length > len(data):
            raise ValueError('Section outside file')
        if typ == 2:
            if entsize != 24 or length % 24 or link >= len(sections):
                raise ValueError('Invalid symbol table')
            strings = sections[link]
            if strings[1] != 3 or strings[4] + strings[5] > len(data):
                raise ValueError('Invalid symbol string table')
            names = data[strings[4]:strings[4]+strings[5]]
            for i in range(off, off + length, 24):
                name, stinfo, other, index, value, symsize = struct.unpack_from('<IBBHQQ', data, i)
                if name >= len(names):
                    raise ValueError('Invalid symbol name')
                symbol = names[name:].split(b'\0', 1)[0]
                if index == 0 and name:
                    raise ValueError('Undefined import: ' + symbol.decode(errors='replace'))
                if symbol == b'nlh_entry':
                    entry_found = index != 0 and value == entry and stinfo & 15 == 2
                if symbol == b'nlh_state_bytes':
                    if not 0 < index < len(sections) or symsize != 4 or value > size - 4 or stinfo & 15 != 1:
                        raise ValueError('Invalid nlh_state_bytes object')
                    section = sections[index]
                    if not (section[2] & 2) or section[2] & 1 or not section[3] <= value <= section[3]+section[5]-4:
                        raise ValueError('State declaration must be in read-only allocated data')
                    state_bytes, = struct.unpack_from('<I', data, offset + value)
    if not entry_found:
        raise ValueError('Missing nlh_entry function symbol')
    if state_bytes is None or not 1 <= state_bytes <= 65536:
        raise ValueError('Declare const uint32_t nlh_state_bytes = sizeof(your_state)')
    return data[offset:offset+size], entry, state_bytes


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True, help='New directory')
    p.add_argument('--name', required=True)
    p.add_argument('--version', type=int, required=True)
    p.add_argument('--interface-abi', type=lambda value: int(value, 0), default=1)
    p.add_argument('--state-abi', type=lambda value: int(value, 0), default=1)
    p.add_argument('--kind', choices=KINDS, default='service',
                   help='core requires a reserved name and the pinned core ABI')
    p.add_argument('--requires', action='append', default=[], metavar='NAME:ABI:MIN_VERSION')
    p.add_argument('--define', action='append', default=[])
    p.add_argument('--include-dir', action='append', type=Path, default=[])
    p.add_argument('--clang', default='clang')
    p.add_argument('--linker', default='ld.lld')
    a = p.parse_args()
    if not re.fullmatch('[a-z0-9_-]{1,31}', a.name): p.error('Invalid module name')
    if not 1 <= a.version <= 0xffffffff: p.error('Version must be 1..UINT32_MAX')
    dependencies = []
    try:
        for requirement in a.requires:
            name, abi, minimum = requirement.split(':')
            dependencies.append((name, int(abi, 0), int(minimum, 0)))
        # Validate metadata before producing build artifacts.
        pack_image(bytes(4), 0, 1, a.name, a.version, a.interface_abi,
                   a.state_abi, a.kind, dependencies)
    except (ValueError, KeyError, struct.error) as error:
        p.error(str(error))
    if any(not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*(?:=[A-Za-z0-9_+-]+)?', d) for d in a.define):
        p.error('Defines must be NAME or NAME=VALUE')
    if not a.source.is_file(): p.error('Source must be an existing file')
    if any(not path.is_dir() for path in a.include_dir):
        p.error('Include directories must exist')
    if a.name in CORE_MODULES:
        definitions = dict(d.partition('=')[::2] for d in a.define)
        domain = definitions.get('NLH_POLICY_DOMAIN')
        if a.source.resolve() == (ROOT / 'core' / 'policy.c').resolve():
            if domain != str(CORE_MODULES[a.name]):
                p.error('core/policy.c requires NLH_POLICY_DOMAIN matching the reserved module name')
    a.out.mkdir(parents=True, exist_ok=False)
    obj, elf = a.out / 'module.o', a.out / 'module.elf'
    includes = [ROOT, ROOT / 'core', *a.include_dir]
    compile_command = [a.clang, '--target=aarch64-none-elf', '-O2', '-ffreestanding',
                    '-fPIC', '-fvisibility=hidden', '-fno-stack-protector', '-fno-builtin',
                    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                    '-mgeneral-regs-only', '-ffixed-x18', '-mno-outline-atomics', '-mbranch-protection=none',
                    '-Wall', '-Wextra', '-Werror',
                    *[arg for path in includes for arg in ('-I', str(path))],
                    *['-D' + d for d in a.define], '-c', str(a.source), '-o', str(obj)]
    subprocess.run(compile_command, check=True)
    check_object(obj.read_bytes())
    link_command = [a.linker, '--no-undefined', '--fatal-warnings', '--orphan-handling=error',
                    '-T', str(ROOT / 'module.ld'), str(obj), '-o', str(elf)]
    subprocess.run(link_command, check=True)
    code, entry, state_bytes = extract_elf(elf.read_bytes())
    image = pack_image(code, entry, state_bytes, a.name, a.version,
                       a.interface_abi, a.state_abi, a.kind, dependencies)
    target = a.out / (a.name + '.nlh')
    target.write_bytes(image)
    report = parse_image(image)
    report.update(source_sha256=hashlib.sha256(a.source.read_bytes()).hexdigest(),
                  image_sha256=hashlib.sha256(image).hexdigest(),
                  compile_command=compile_command, link_command=link_command,
                  kit_inputs={name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                              for name in ('native_hotload.h', 'native_hotload_core.h',
                                           'core/native_core_algorithms.h', 'module.ld',
                                           'nlh_format.py', 'build-module.py')},
                  hardware_tested=False)
    (a.out / 'module.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'{target}: {len(image)} bytes, version={a.version}, sha256={hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
