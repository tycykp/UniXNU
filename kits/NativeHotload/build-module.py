"""Build a position-independent AArch64 NLH2 module on Linux/WSL.

The ELF is a build artifact only. The kernel receives a checked header plus
RX code, and resolves no symbols from the kernel or another module.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import shlex
import shutil
import tempfile
from nlh_format import CORE_MODULES, MAX_CODE, KINDS, pack_image, parse_image

ROOT = Path(__file__).resolve().parent


def json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(',', ':')).encode()).hexdigest()


def dependency_paths(path):
    text = path.read_text().replace('\\\n', '').replace('$$', '$')
    # Both preflight and compilation use -MT with this fixed, path-free target.
    # This avoids mistaking a colon in an escaped filename for the separator.
    prefix = 'nlh-module:'
    if not text.startswith(prefix):
        raise ValueError('Unexpected compiler dependency target')
    paths = {Path(value).resolve() for value in shlex.split(text[len(prefix):])}
    if not paths or any(not value.is_file() for value in paths):
        raise ValueError('Missing compiler dependency input')
    return paths


def input_hashes(paths, roots):
    inputs = {}
    for path in sorted(paths):
        for label, directory in roots:
            if path.is_relative_to(directory):
                key = label + '/' + path.relative_to(directory).as_posix()
                if key in inputs:
                    raise ValueError('Ambiguous module source identity: ' + key)
                inputs[key] = hashlib.sha256(path.read_bytes()).hexdigest()
                break
        else:
            raise ValueError('Undeclared module header dependency: ' + str(path))
    return inputs


def require_unchanged(paths, roots, expected):
    if input_hashes(paths, roots) != expected:
        raise ValueError('Module source/header changed during compilation; discard this build')


def kit_hashes():
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in ('native_hotload.h', 'native_hotload_core.h',
                         'core/native_core_algorithms.h', 'module.ld',
                         'nlh_format.py', 'build-module.py')}


def tool_identity(tool):
    return subprocess.check_output([tool, '--version'], text=True).strip()


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
    a.source = a.source.resolve()
    a.out = a.out.absolute()
    includes = [ROOT.resolve(), (ROOT / 'core').resolve(),
                *[path.resolve() for path in a.include_dir]]
    roots = [('kit', ROOT.resolve()), ('source', a.source.parent),
             *[('include_' + str(i), path.resolve()) for i, path in enumerate(a.include_dir)]]
    compiler = shutil.which(a.clang)
    linker = shutil.which(a.linker)
    if not compiler or not linker:
        p.error('Compiler and linker must resolve to existing executables')
    compiler_identity, linker_identity = tool_identity(compiler), tool_identity(linker)
    kit_inputs = kit_hashes()
    compile_flags = ['--target=aarch64-none-elf', '-O2', '-ffreestanding',
                    '-fPIC', '-fvisibility=hidden', '-fno-stack-protector', '-fno-builtin',
                    '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                    '-mgeneral-regs-only', '-ffixed-x18', '-mno-outline-atomics', '-mbranch-protection=none',
                    '-Wall', '-Wextra', '-Werror',
                    *[arg for path in includes for arg in ('-I', str(path))],
                    *['-D' + d for d in a.define]]
    link_flags = ['--no-undefined', '--fatal-warnings', '--orphan-handling=error',
                  '-T', str(ROOT / 'module.ld')]
    a.out.parent.mkdir(parents=True, exist_ok=True)
    # A sibling exclusive lock serializes cooperating builders without ever
    # removing or overwriting an existing output. A crashed build leaves its
    # lock for explicit inspection rather than automatically declaring it dead.
    lock_path = a.out.with_name('.' + a.out.name + '.build-lock')
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        with os.fdopen(lock_fd, 'w', encoding='utf-8') as lock:
            lock.write(str(os.getpid()) + '\n')
        if os.path.lexists(a.out):
            raise FileExistsError('Output directory already exists: ' + str(a.out))
        with tempfile.TemporaryDirectory(prefix='.' + a.out.name + '.stage-',
                                         dir=a.out.parent) as temporary:
            stage = Path(temporary) / 'artifacts'
            stage.mkdir()
            obj, elf = stage / 'module.o', stage / 'module.elf'
            dependency_file, before_file = stage / 'module.d', stage / 'module-before.d'
            dependency_command = [compiler, *compile_flags, '-MM', '-MT', 'nlh-module',
                                  '-MF', str(before_file), str(a.source)]
            subprocess.run(dependency_command, check=True)
            before_paths = dependency_paths(before_file)
            if a.source not in before_paths:
                raise ValueError('Compiler dependency preflight omitted the module source')
            source_inputs = input_hashes(before_paths, roots)
            primary = input_hashes({a.source}, roots)
            if any(source_inputs.get(key) != value for key, value in primary.items()):
                raise ValueError('Module source changed during preflight hashing')
            source_sha256 = next(iter(primary.values()))
            build_inputs = {'source': str(a.source), 'source_inputs': source_inputs,
                'compile_flags': compile_flags, 'link_flags': link_flags,
                'compiler': compiler, 'linker': linker,
                'compiler_identity': compiler_identity, 'linker_identity': linker_identity,
                'kit_inputs': kit_inputs, 'image': {'name': a.name, 'version': a.version,
                    'interface_abi': a.interface_abi, 'state_abi': a.state_abi,
                    'kind': a.kind, 'dependencies': [list(dep) for dep in dependencies]}}
            compile_command = [compiler, *compile_flags, '-MMD', '-MT', 'nlh-module',
                               '-MF', str(dependency_file), '-c', str(a.source), '-o', str(obj)]
            subprocess.run(compile_command, check=True)
            if dependency_paths(dependency_file) != before_paths:
                raise ValueError('Compiler dependency set changed after preflight')
            require_unchanged(before_paths, roots, source_inputs)
            if kit_hashes() != kit_inputs:
                raise ValueError('Module tooling or ABI header changed during compilation')
            check_object(obj.read_bytes())
            link_command = [linker, *link_flags, str(obj), '-o', str(elf)]
            subprocess.run(link_command, check=True)
            code, entry, state_bytes = extract_elf(elf.read_bytes())
            image = pack_image(code, entry, state_bytes, a.name, a.version,
                               a.interface_abi, a.state_abi, a.kind, dependencies)
            target = stage / (a.name + '.nlh')
            target.write_bytes(image)
            if target.read_bytes() != image:
                raise ValueError('Packaged module image readback differs from verified bytes')
            report = parse_image(target.read_bytes())
            report.update(source_sha256=source_sha256,
                image_sha256=hashlib.sha256(image).hexdigest(),
                dependency_command=dependency_command,
                compile_command=compile_command, link_command=link_command,
                source_inputs=source_inputs, source_inputs_sha256=json_digest(source_inputs),
                compiler_identity=compiler_identity, linker_identity=linker_identity,
                kit_inputs=kit_inputs, build_inputs=build_inputs,
                build_inputs_sha256=json_digest(build_inputs),
                source_consistency='preflight-and-postcompile-dependency-set-and-sha256',
                atomic_output='verified-sibling-directory-rename-with-exclusive-build-lock',
                artifacts={path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (obj, elf, dependency_file, before_file, target)},
                hardware_tested=False)
            manifest = stage / 'module.json'
            manifest.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            if json.loads(manifest.read_text(encoding='utf-8')) != report:
                raise ValueError('Module manifest readback differs from verified metadata')
            require_unchanged(before_paths, roots, source_inputs)
            if kit_hashes() != kit_inputs or tool_identity(compiler) != compiler_identity or \
                    tool_identity(linker) != linker_identity:
                raise ValueError('Module toolchain/tooling changed before publication')
            if os.path.lexists(a.out):
                raise FileExistsError('Output appeared during build: ' + str(a.out))
            if not stage.resolve().is_relative_to(a.out.parent.resolve()):
                raise ValueError('Staging output escaped the requested parent directory')
            # All files become visible together on the same filesystem, only
            # after object/ELF/image/manifest and source binding checks pass.
            stage.rename(a.out)
    finally:
        lock_path.unlink()
    target = a.out / (a.name + '.nlh')
    print(f'{target}: {len(image)} bytes, version={a.version}, sha256={hashlib.sha256(image).hexdigest()}')


if __name__ == '__main__':
    main()
