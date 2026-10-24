"""Shared NLH2 image contract for the builder and USB client."""
import hashlib
import re
import struct

ABI = 2
MAGIC = 0x32484c4e
MAX_CODE = 256 * 1024
MAX_STATE = 64 * 1024
MAX_DEPENDENCIES = 8
PREFIX = struct.Struct('<12I32s')
DEPENDENCY = struct.Struct('<32sII')
HEADER_BYTES = PREFIX.size + MAX_DEPENDENCIES * DEPENDENCY.size + 32
KINDS = {'service': 0, 'driver': 1, 'core': 2}
CORE_INTERFACE = 0x434f0001
CORE_STATE_ABI = 1
CORE_MODULES = {'xnu-sched': 1, 'xnu-vm': 2, 'xnu-vfs': 3}


def uint32(value):
    return isinstance(value, int) and 1 <= value <= 0xffffffff


def name_bytes(value):
    if not re.fullmatch('[a-z0-9_-]{1,31}', value):
        raise ValueError('Names require 1..31 lowercase letters, digits, underscore or hyphen')
    return value.encode('ascii').ljust(32, b'\0')


def read_name(raw):
    if not re.fullmatch(b'[a-z0-9_-]{1,31}\x00+', raw):
        raise ValueError('Invalid module/dependency name')
    return raw.split(b'\0', 1)[0].decode('ascii')


def parse_image(data):
    if not HEADER_BYTES + 4 <= len(data) <= HEADER_BYTES + MAX_CODE:
        raise ValueError('Invalid NLH2 image length')
    magic, abi, head, size, state, entry, version, interface, state_abi, count, kind, reserved, name = PREFIX.unpack_from(data)
    if (magic, abi, head, reserved) != (MAGIC, ABI, HEADER_BYTES, 0) or size != len(data) - HEADER_BYTES:
        raise ValueError('Invalid NLH2 ABI/header (NLH1 is not compatible)')
    if not 1 <= state <= MAX_STATE or not version or not interface or not state_abi or entry & 3 or entry > size - 4:
        raise ValueError('Invalid state/version/interface/entry')
    if count > MAX_DEPENDENCIES or kind not in KINDS.values():
        raise ValueError('Invalid dependency count or module kind')
    name = read_name(name)
    dependencies = []
    seen = {name}
    for i in range(MAX_DEPENDENCIES):
        raw = data[PREFIX.size + i * DEPENDENCY.size:PREFIX.size + (i + 1) * DEPENDENCY.size]
        if i >= count:
            if any(raw): raise ValueError('Unused dependency fields must be zero')
            continue
        dep_name, dep_abi, dep_min = DEPENDENCY.unpack(raw)
        dep_name = read_name(dep_name)
        if dep_name in seen or not dep_abi or not dep_min:
            raise ValueError('Duplicate/self dependency or zero dependency version')
        seen.add(dep_name)
        dependencies.append({'name': dep_name, 'interface_abi': dep_abi, 'minimum_version': dep_min})
    if hashlib.sha256(data[:HEADER_BYTES - 32] + data[HEADER_BYTES:]).digest() != data[HEADER_BYTES - 32:HEADER_BYTES]:
        raise ValueError('NLH2 digest mismatch')
    domain = CORE_MODULES.get(name)
    if kind == KINDS['core'] or domain is not None:
        if (domain is None or kind != KINDS['core'] or interface != CORE_INTERFACE or
                state_abi != CORE_STATE_ABI or dependencies):
            raise ValueError('Core modules require a reserved xnu-sched/xnu-vm/xnu-vfs name, '
                             'core kind, the pinned core interface/state ABI and no dependencies')
    report = {'abi': abi, 'name': name, 'version': version, 'interface_abi': interface,
              'state_abi': state_abi, 'kind': kind, 'dependencies': dependencies,
              'state_bytes': state, 'code_bytes': size, 'entry_offset': entry}
    if domain is not None:
        report['core_domain'] = domain
    return report


def pack_image(code, entry, state, name, version, interface, state_abi, kind, dependencies):
    if not all(uint32(v) for v in (version, interface, state_abi)):
        raise ValueError('Version and interface/state ABI must be 1..UINT32_MAX')
    if len(dependencies) > MAX_DEPENDENCIES:
        raise ValueError('At most eight dependencies per module')
    head = PREFIX.pack(MAGIC, ABI, HEADER_BYTES, len(code), state, entry, version, interface,
                       state_abi, len(dependencies), KINDS[kind], 0, name_bytes(name))
    for dep_name, dep_abi, dep_min in dependencies:
        if not uint32(dep_abi) or not uint32(dep_min): raise ValueError('Invalid dependency version')
        head += DEPENDENCY.pack(name_bytes(dep_name), dep_abi, dep_min)
    head += bytes((MAX_DEPENDENCIES - len(dependencies)) * DEPENDENCY.size)
    image = head + hashlib.sha256(head + code).digest() + code
    parse_image(image)
    return image
