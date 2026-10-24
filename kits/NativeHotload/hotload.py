"""Acknowledged NLH2 USB operations, reusable within one locked session.

Requires pyserial. Cooperating serial/USB tools use DeviceMutex. Unknown
mutating results are queried with status/list, never automatically retried.
"""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import re
import secrets
import sys
import threading
import time
from nlh_format import ABI, parse_image
from serial_mutex import DeviceMutex, DEFAULT_DEVICE_KEY


def image_bytes(path):
    data = Path(path).read_bytes()
    return data, parse_image(data)


class DeviceError(RuntimeError):
    def __init__(self, message, response):
        super().__init__(message)
        self.response = response


class UnknownOutcome(RuntimeError):
    def __init__(self, operation, cause, resolution):
        super().__init__(f'{operation}: outcome unknown; status/list recorded; no retry: {cause}')
        self.operation, self.resolution = operation, resolution


class DiagnosticBlocked(RuntimeError):
    def __init__(self, message, response):
        super().__init__(message)
        self.response = response


DIAGNOSTICS = {
    'gpu.status': 'GIULIA GPU:', 'wifi.status': 'GIULIA WIFI:',
    'audio.status': 'GIULIA AUDIO:', 'audio.preflight': 'GIULIA AUDIO:',
    'modem.status': 'GIULIA MODEM:', 'audio.proto': 'GIULIA AUDIO:',
    'modem.proto': 'GIULIA MODEM PROTO:',
}


def fields(text):
    return {k: (int(v,16) if re.fullmatch('-?0x[0-9a-f]+',v) else int(v))
            if re.fullmatch('-?(?:0x[0-9a-f]+|[0-9]+)', v) and not k.endswith('sha256') else v
            for k, v in re.findall(r'([a-z_][a-z0-9_]*)=([a-z0-9_,.-]+)', text)}


class Wire:
    def __init__(self, port, log, timeout):
        self.port, self.log, self.timeout = port, log, timeout
        self.buffer = bytearray()
        self.modules, self.core, self.gateway = [], [], []
        self.pending = []
        self.lines = []
        self.sequence = self.last_reply_sequence = 0
        self.owner_thread = threading.get_ident()

    def send(self, command):
        if threading.get_ident() != self.owner_thread:
            raise RuntimeError('One Wire has one owner; serialize all serial commands')
        encoded = command.encode('ascii')
        if len(encoded) > 63 or b'\n' in encoded or b'\r' in encoded:
            raise ValueError('Command exceeds terminal protocol')
        self.port.write(encoded + b'\n')
        self.port.flush()

    def _line(self, raw):
        line = raw.decode('ascii', errors='replace').strip()
        self.sequence += 1
        self.lines.append((self.sequence,line))
        if len(self.lines)>4096: del self.lines[:len(self.lines)-4096]
        for marker, records in (('module', self.modules), ('core', self.core),
                                ('gateway', self.gateway)):
            match = re.search(r'NLH ' + marker + r' (.+)$', line)
            if match:
                records.append(fields(match[1]))
        # A complete reply may be concatenated to a console prefix. Retain the
        # exact marker, numeric tail and token/nonce correlation checks.
        match = re.search(r'NLH (ack|done|status|memory) ((?:[a-z_]+=-?[0-9]+ ?)+)$', line)
        if 'NLH disabled ' in line:
            raise RuntimeError('Hotload is disabled or unsupported by this kernel')
        if match:
            self.pending.append((match[1], fields(match[2]), self.sequence))
            if len(self.pending) > 64:
                raise RuntimeError('Too many unmatched replies; protocol desynchronized')

    def receive(self, kind, token=None, nonce=None):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            while b'\n' in self.buffer:
                raw, _, rest = self.buffer.partition(b'\n')
                self.buffer = bytearray(rest)
                self._line(raw)
            for index, (record_kind, record, sequence) in enumerate(self.pending):
                if (record_kind == kind and (token is None or record.get('token') == token)
                        and (nonce is None or record.get('nonce') == nonce)):
                    self.last_reply_sequence = sequence
                    if kind == 'status':
                        # A nonce-correlated status is a transport ordering
                        # barrier. Older ACK/DONE records must not satisfy the
                        # next query after an unknown mutating outcome.
                        self.pending = self.pending[index + 1:]
                    else:
                        self.pending.pop(index)
                    return record
            raw = self.port.read(4096)
            if raw:
                self.log.write(raw)
                self.log.flush()
                self.buffer.extend(raw)
                if len(self.buffer) > 65536:
                    raise RuntimeError('Unbounded device line; protocol desynchronized')
        raise TimeoutError('No matching reply; operation may still be running')

    def wait_lines(self, ready):
        deadline=time.monotonic()+self.timeout
        while time.monotonic()<deadline:
            while b'\n' in self.buffer:
                raw,_,rest=self.buffer.partition(b'\n')
                self.buffer=bytearray(rest)
                self._line(raw)
            if ready(): return
            raw=self.port.read(4096)
            if raw:
                self.log.write(raw); self.log.flush(); self.buffer.extend(raw)
                if len(self.buffer)>65536: raise RuntimeError('Unbounded diagnostic line')
        raise TimeoutError('Diagnostic did not emit its complete response')

    def ack(self, command, token=None, nonce=None):
        self.send(command)
        response = self.receive('ack', token, nonce)
        if response.get('error', -1):
            raise DeviceError(f'Device refused {command.split()[0]}: {response}', response)
        return response


class Client:
    """Synchronous client. Caller owns the device mutex for its whole lifetime."""
    def __init__(self, wire):
        self.wire = wire

    def status(self):
        nonce = secrets.randbelow(0xffffffff) + 1
        self.wire.send(f'hl.status {nonce}')
        result = self.wire.receive('status', nonce=nonce)
        if result.get('abi') != ABI:
            raise RuntimeError('Kernel/client ABI mismatch')
        return result

    def memory(self):
        nonce = secrets.randbelow(0xffffffff) + 1
        self.wire.send(f'hl.memory {nonce}')
        return self.wire.receive('memory', nonce=nonce)

    def query(self, command):
        if command not in ('list', 'core', 'gateway'):
            raise ValueError('Unsupported diagnostic query')
        records = getattr(self.wire, 'modules' if command == 'list' else command)
        records.clear()
        if command == 'gateway':
            nonce = secrets.randbelow(0xffffffff) + 1
            self.wire.ack(f'hl.gateway {nonce}', nonce=nonce)
        else:
            self.wire.ack('hl.' + command)
        return list(records)

    def list(self): return self.query('list')
    def core(self): return self.query('core')
    def gateway(self): return self.query('gateway')

    def diagnostic(self, command):
        """Fixed read-only/cache and isolated protocol fixtures; never HW proof."""
        if command not in DIAGNOSTICS:
            raise ValueError('Diagnostic command is outside the fixed read-only allowlist')
        try:
            self.status()  # discard old ACK/DONE and old diagnostic output first
        except (OSError,RuntimeError,TimeoutError) as error:
            raise UnknownOutcome(command+'-readiness',error,self.resolve_unknown()) from error
        cursor=self.wire.sequence
        prefix=DIAGNOSTICS[command]
        def rows():
            result=[]
            for sequence,line in self.wire.lines:
                if sequence<=cursor or prefix not in line: continue
                tail=line[line.rfind(prefix)+len(prefix):].strip()
                result.append(dict(fields(tail),_text=tail))
            return result
        def completed():
            records=rows()
            if any(isinstance(row.get('rejected'),str) or 'offline command=' in row['_text'] or
                   'requires giulia_debug=1' in row['_text'] for row in records): return True
            if command=='audio.proto': return any(row.get('cases')==20 and 'passed_mask' in row and 'changed' in row for row in records)
            if command=='modem.proto': return any(row.get('cases')==22 and 'native_delta' in row and 'state_unchanged' in row for row in records)
            if command=='audio.status': return any(row.get('proto_domain')==3 and 'local_changed' in row for row in records)
            if command=='audio.preflight':
                return (any(row.get('checks_only')==1 or row.get('command')=='audio.preflight' for row in records) and
                        any(row.get('proto_domain')==3 and 'local_changed' in row for row in records))
            if command=='wifi.status': return any('hotload epoch=' in row['_text'] or 'boot snapshot unavailable' in row['_text'] for row in records)
            if command=='modem.status': return any(row.get('slot')==1 and 'sms' in row for row in records)
            if command=='gpu.status':
                return (any(row.get('command')=='gpu.status' and row.get('service')=='unavailable' for row in records) or
                        (any(row.get('command')=='gpu.status' for row in records) and
                         any('firmware_power_valid' in row and 'qmp_available' in row for row in records)))
            return False
        self.wire.send(command)
        try:
            self.wire.wait_lines(completed)
            self.status()  # ordered completion boundary after the final marker
        except (OSError,RuntimeError,TimeoutError) as error:
            partial=rows()
            resolution=self.resolve_unknown()
            resolution['diagnostic_rows']=partial
            raise UnknownOutcome(command,error,resolution) from error
        records=rows()
        result={'command':command,'scope':'synthetic' if command.endswith('.proto') else 'cached_status',
                'rows':records,'complete':True,'hardware_verified':False,
                'snapshot_consistency':'approximate' if not command.endswith('.proto') else 'isolated-fixture'}
        if any(isinstance(row.get('rejected'),str) or 'offline command=' in row['_text'] or
               'requires giulia_debug=1' in row['_text'] for row in records):
            raise DiagnosticBlocked('Diagnostic refused; no fixture/hardware success',result)
        if command.endswith('.proto'):
            summaries=[row for row in records if 'cases' in row]
            if len(summaries)!=1: raise RuntimeError('Fixture emitted duplicate or missing summary')
            summary=summaries[0]
            result['summary']=summary
            if command=='audio.proto':
                if summary.get('counter_exact')!=1:
                    raise DiagnosticBlocked('Audio live traffic prevents exact fixture accounting',result)
                expected={'synthetic':1,'hardware_verified':0,'counter_exact':1,'cases':20,
                          'passed_mask':0xfffff,'expected_mask':0xfffff,'failed_mask':0,'attempted':16,'changed':0}
                if any(summary.get(key)!=value for key,value in expected.items()):
                    raise RuntimeError('Audio fixture bitmap/exact-counter summary failed')
                if summary.get('adopted',-1)+summary.get('fallback',-1)!=16 or not 0<=summary.get('rejected',-1)<=summary.get('fallback',-1):
                    raise RuntimeError('Audio fixture did not account for all sixteen real protocol decisions')
            else:
                expected={'source':'fixture','cases':22,'passed':22,'failed':0,'native_delta':0,'state_unchanged':1,'hardware_event':0}
                cases=[row for row in records if 'case' in row]
                if len(cases)!=22 or any(summary.get(key)!=value for key,value in expected.items()):
                    raise RuntimeError('Modem fixture did not emit all twenty-two cases and its complete summary')
                keys=[(row['case'],row.get('malformed',0)) for row in cases]
                if len(set(keys))!=22: raise RuntimeError('Modem fixture repeated a case')
                if any(row.get('source')!='fixture' or row.get('pass')!=1 or row.get('expected')!=row.get('actual') or
                       row.get('hardware_event')!=0 or row.get('module_delta',-1)+row.get('fallback_delta',-1)!=1 for row in cases):
                    raise RuntimeError('Modem fixture case failed or lacks real protocol accounting')
                if summary.get('module_delta',-1)+summary.get('fallback_delta',-1)!=22:
                    raise RuntimeError('Modem fixture summary omitted real protocol decisions')
        return result

    def resolve_unknown(self):
        result = {}
        try:
            result['status'] = self.status()
        except (OSError, RuntimeError, TimeoutError) as error:
            result['status_error'] = str(error)
            return result
        if not result['status'].get('busy'):
            try:
                result['modules'] = self.query('list')
            except (OSError, RuntimeError, TimeoutError) as error:
                result['list_error'] = str(error)
        else:
            result['list_blocked'] = 'runtime busy; no mutating retry'
        return result

    def _complete(self, command, token=None, expected=None):
        try:
            ack = self.wire.ack(command, token)
            if not ack.get('busy'):
                raise RuntimeError('Device did not queue the operation')
            done = self.wire.receive('done', token=ack['token'])
        except DeviceError:
            raise
        except (OSError, RuntimeError, TimeoutError) as error:
            raise UnknownOutcome(command.split()[0], error, self.resolve_unknown()) from error
        if done.get('error', -1) or done.get('state') == 3:
            raise DeviceError('Module operation failed', done)
        if expected and any(done.get(key) != value for key, value in expected.items()):
            raise DeviceError('Operation finished with unexpected module state', done)
        return done

    def load(self, path):
        data, _ = image_bytes(path)
        return self.load_bytes(data)

    def load_bytes(self, data):
        """Upload one immutable, validated image snapshot (no path reread)."""
        data = bytes(data)
        metadata = parse_image(data)
        try:
            ack = self.wire.ack(f'hl.begin {len(data)}')
        except DeviceError:
            raise
        except (OSError, RuntimeError, TimeoutError) as error:
            raise UnknownOutcome('upload-begin', error, self.resolve_unknown()) from error
        token = ack['token']
        try:
            for offset in range(0, len(data), 16):
                part = data[offset:offset + 16]
                ack = self.wire.ack(f'hl.data {token} {offset} {part.hex()}', token)
                if ack.get('received') != offset + len(part):
                    raise RuntimeError('Device upload offset mismatch')
        except (OSError, RuntimeError, TimeoutError) as error:
            # Preserve staging for explicit diagnosis/abort. Never guess if a
            # timed-out packet was accepted or resend an offset.
            raise UnknownOutcome('upload', error, self.resolve_unknown()) from error
        return self._complete(f'hl.commit {token}', token,
                              {'state': 1, 'active': metadata['version']})

    def call(self, name, value):
        validate_name(name)
        if not 0 <= value < 1 << 64:
            raise ValueError('Argument must be uint64')
        return self._complete(f'hl.call {name} {value}')

    def unload(self, name):
        validate_name(name)
        return self._complete(f'hl.unload {name}', expected={'state': 0, 'active': 0})

    def abort(self, token):
        if not 1 <= token <= 0xffffffff:
            raise ValueError('Token must be uint32')
        return self.wire.ack(f'hl.abort {token}', token)


def validate_name(name):
    if not re.fullmatch('[a-z0-9_-]{1,31}', name):
        raise ValueError('Invalid module name')


@contextmanager
def open_session(port, log, timeout=30, lock_timeout=0, device_key=DEFAULT_DEVICE_KEY):
    """Acquire physical-device ownership BEFORE opening pyserial; no reopens."""
    import serial
    path = Path(log)
    path.parent.mkdir(parents=True, exist_ok=True)
    with DeviceMutex(device_key, lock_timeout):
        with path.open('xb') as capture:
            with serial.Serial(port, 115200, timeout=0.1, write_timeout=5) as handle:
                client = Client(Wire(handle, capture, timeout))
                client.status()
                client.wire.modules.clear()
                client.wire.core.clear()
                client.wire.gateway.clear()
                client.wire.pending.clear()  # stale records before readiness nonce
                yield client


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--log', type=Path, required=True, help='New raw capture file')
    p.add_argument('--timeout', type=float, default=30)
    p.add_argument('--lock-timeout', type=float, default=0)
    p.add_argument('--device-key', default=DEFAULT_DEVICE_KEY)
    p.add_argument('--json', action='store_true', help='One machine-readable JSON result')
    sub = p.add_subparsers(dest='command', required=True)
    load = sub.add_parser('load'); load.add_argument('image', type=Path)
    for command in ('status', 'list', 'core', 'memory', 'gateway'):
        sub.add_parser(command)
    unload = sub.add_parser('unload'); unload.add_argument('name')
    call = sub.add_parser('call'); call.add_argument('name'); call.add_argument('value', type=int)
    abort = sub.add_parser('abort'); abort.add_argument('token', type=int)
    diagnostic=sub.add_parser('diagnostic'); diagnostic.add_argument('diagnostic_command',choices=tuple(DIAGNOSTICS))
    a = p.parse_args()
    if not 0 < a.timeout <= 3600:
        p.error('Timeout must be 0..3600 seconds')
    result = {'command': a.command, 'status': 'fail', 'raw_log': str(a.log.resolve())}
    try:
        if a.command == 'load':
            image_bytes(a.image)
        with open_session(a.port, a.log, a.timeout, a.lock_timeout, a.device_key) as client:
            if a.command in ('list', 'core', 'gateway'):
                response = client.query(a.command)
            elif a.command == 'load':
                response = client.load(a.image)
            elif a.command == 'call':
                response = client.call(a.name, a.value)
            elif a.command == 'unload':
                response = client.unload(a.name)
            elif a.command == 'abort':
                response = client.abort(a.token)
            elif a.command == 'diagnostic':
                response = client.diagnostic(a.diagnostic_command)
            else:
                response = getattr(client, a.command)()
            result.update(status='pass', response=response)
    except (ValueError, RuntimeError, TimeoutError, OSError) as error:
        result['error'] = str(error)
        if isinstance(error, UnknownOutcome):
            result.update(status='blocked', resolution=error.resolution)
        if isinstance(error, DeviceError):
            result['response'] = error.response
        if isinstance(error, DiagnosticBlocked):
            result.update(status='blocked',response=error.response)
    if a.json:
        print(json.dumps(result, sort_keys=True))
    elif result['status'] == 'pass':
        response = result['response']
        if isinstance(response, list):
            for row in response: print(row)
        else: print(response)
    else:
        print(result['error'], file=sys.stderr)
    return 0 if result['status'] == 'pass' else 2 if result['status'] == 'blocked' else 1


if __name__ == '__main__':
    raise SystemExit(main())
