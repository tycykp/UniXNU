"""Upload native modules over the existing 63-byte USB diagnostic line protocol.

Requires pyserial. Close other terminals before opening the port. Timeouts do
not retry a commit or unload; query status to resolve an unknown outcome.
"""
import argparse
from pathlib import Path
import re
import secrets
import sys
import time
from nlh_format import ABI, parse_image


def image_bytes(path):
    data = path.read_bytes()
    return data, parse_image(data)


class Wire:
    def __init__(self, port, log, timeout):
        self.port, self.log, self.timeout = port, log, timeout
        self.buffer = bytearray()
        self.modules = []
        self.core = []

    def send(self, command):
        encoded = command.encode('ascii')
        if len(encoded) > 63 or b'\n' in encoded or b'\r' in encoded:
            raise ValueError('Command exceeds terminal protocol')
        self.port.write(encoded + b'\n')
        self.port.flush()

    def receive(self, kind, token=None, nonce=None):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            while b'\n' in self.buffer:
                raw, _, rest = self.buffer.partition(b'\n')
                self.buffer = bytearray(rest)
                line = raw.decode('ascii', errors='replace').strip()
                module = re.search(r'NLH module (slot=[0-9]+ name=[a-z0-9_-]+ .+)$', line)
                if module:
                    self.modules.append(dict(re.findall(r'([a-z_]+)=([a-z0-9_-]+)', module[1])))
                core = re.search(r'NLH core (.+)$', line)
                if core:
                    self.core.append(dict(re.findall(r'([a-z_]+)=([a-z0-9_,.-]+)', core[1])))
                # Concurrent console output may leave a partial prefix without
                # whitespace before NLH. Keep the exact marker, numeric record,
                # line end and token/nonce checks; "NLH module:" is not a reply.
                match = re.search(r'NLH (ack|done|status|memory) ((?:[a-z_]+=-?[0-9]+ ?)+)$', line)
                if 'NLH disabled ' in line:
                    raise RuntimeError('Hotload is disabled or not supported by this kernel')
                if match and match[1] == kind:
                    fields = {k: int(v) for k, v in re.findall(r'([a-z_]+)=(-?[0-9]+)', match[2])}
                    if ((token is None or fields.get('token') == token) and
                            (nonce is None or fields.get('nonce') == nonce)):
                        return fields
            raw = self.port.read(4096)
            if raw:
                self.log.write(raw); self.log.flush(); self.buffer.extend(raw)
                if len(self.buffer) > 65536:
                    raise RuntimeError('Unbounded device line; protocol desynchronized')
        raise TimeoutError('No matching reply; operation may still be running. Reconnect and use status.')

    def ack(self, command, token=None):
        self.send(command)
        response = self.receive('ack', token)
        if response.get('error', -1):
            raise RuntimeError(f'Device refused {command.split()[0]}: {response}')
        return response


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--log', type=Path, required=True, help='New raw capture file')
    p.add_argument('--timeout', type=float, default=30)
    sub = p.add_subparsers(dest='command', required=True)
    load = sub.add_parser('load'); load.add_argument('image', type=Path)
    sub.add_parser('status'); sub.add_parser('list'); sub.add_parser('core'); sub.add_parser('memory')
    unload = sub.add_parser('unload'); unload.add_argument('name')
    call = sub.add_parser('call'); call.add_argument('name'); call.add_argument('value', type=int)
    abort = sub.add_parser('abort'); abort.add_argument('token', type=int)
    a = p.parse_args()
    if not 0 < a.timeout <= 3600: p.error('Timeout must be 0..3600 seconds')
    if a.command == 'call' and not 0 <= a.value < 1 << 64: p.error('Argument must be uint64')
    if a.command == 'abort' and not 1 <= a.token <= 0xffffffff: p.error('Token must be uint32')
    if a.command in ('call', 'unload') and not re.fullmatch('[a-z0-9_-]{1,31}', a.name):
        p.error('Invalid module name')
    data, metadata = image_bytes(a.image) if a.command == 'load' else (None, None)
    import serial
    a.log.parent.mkdir(parents=True, exist_ok=True)
    with a.log.open('xb') as log, serial.Serial(a.port, 115200, timeout=0.1, write_timeout=5) as port:
        wire = Wire(port, log, a.timeout)
        # Readiness query, without changing module or screen/shell state.
        nonce = secrets.randbelow(0xffffffff) + 1
        wire.send(f'hl.status {nonce}')
        status = wire.receive('status', nonce=nonce)
        if status.get('abi') != ABI: raise RuntimeError('Kernel/client ABI mismatch')
        wire.modules.clear()  # Discard old console records preceding the nonce reply.
        wire.core.clear()
        if a.command == 'status': print(status); return
        if a.command == 'memory':
            wire.send(f'hl.memory {nonce}')
            print(wire.receive('memory', nonce=nonce))
            return
        if a.command == 'core':
            # Core counters are safe to read while a replacement is running.
            wire.ack('hl.core', status['token'])
            for domain in wire.core: print(domain)
            return
        if status.get('busy'):
            raise RuntimeError(f'Runtime busy: {status}')
        if a.command == 'list':
            wire.ack('hl.list')
            for module in wire.modules: print(module)
            return
        if a.command == 'abort':
            print(wire.ack(f'hl.abort {a.token}', a.token)); return
        if a.command == 'load':
            ack = wire.ack(f'hl.begin {len(data)}')
            token = ack['token']
            try:
                for offset in range(0, len(data), 16):
                    part = data[offset:offset+16]
                    ack = wire.ack(f'hl.data {token} {offset} {part.hex()}', token)
                    if ack['received'] != offset + len(part):
                        raise RuntimeError('Device upload offset mismatch')
                # Once commit is sent, an exception leaves the outcome unknown.
                # Do not automatically abort or issue another commit.
            except (Exception, KeyboardInterrupt):
                try: wire.ack(f'hl.abort {token}', token)
                except Exception: pass
                raise
            ack = wire.ack(f'hl.commit {token}', token)
        else:
            ack = wire.ack(f'hl.unload {a.name}' if a.command == 'unload' else f'hl.call {a.name} {a.value}')
            token = ack['token']
        if not ack.get('busy'):
            raise RuntimeError('Device did not queue the operation')
        done = wire.receive('done', token)
        print(done)
        if done.get('error', -1) or done.get('state') == 3:
            raise RuntimeError('Module operation failed; inspect capture and status')
        if a.command == 'load' and (done.get('state') != 1 or done.get('active') != metadata['version']):
            raise RuntimeError('Replacement did not become active')
        if a.command == 'unload' and (done.get('state') != 0 or done.get('active')):
            raise RuntimeError('Module remains loaded')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, TimeoutError, OSError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
