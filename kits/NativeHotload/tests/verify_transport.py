"""Offline protocol/mutex checks. No serial port is opened."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hotload import Wire, Client, UnknownOutcome, DiagnosticBlocked
from nlh_format import pack_image
from serial_mutex import DeviceMutex


class Port:
    def __init__(self, lost_commit=False):
        self.incoming = bytearray()
        self.commands = []
        self.received, self.token, self.lost_commit = 0, 0, lost_commit

    def write(self, raw):
        command = raw.decode().strip()
        self.commands.append(command)
        print('TX', command)
        words = command.split()
        response = ''
        if words[0] == 'hl.status':
            nonce = int(words[1])
            response = (f'consoleNLH status abi=2 nonce={nonce + 1} token={self.token} busy=0\n'
                        f'prefixNLH status abi=2 nonce={nonce} token={self.token} busy=0 provider=1\n')
        elif words[0] == 'hl.begin':
            self.token += 1
            response = f'NLH ack token={self.token} error=0 busy=0 received=0\n'
        elif words[0] == 'hl.data':
            assert int(words[2]) == self.received
            self.received += len(words[3]) // 2
            response = f'NLH ack token={self.token} error=0 busy=0 received={self.received}\n'
        elif words[0] == 'hl.commit':
            if not self.lost_commit:
                # Scheduler may finish before the CLI consumes ACK.
                response = (f'consoleNLH done token={self.token} error=0 state=1 active=1 result=0\n'
                            f'NLH ack token={self.token} error=0 busy=1 received={self.received}\n')
        elif words[0] == 'hl.list':
            response = (f'NLH module slot=0 name=test state=1 active=1 generation=1\n'
                        f'NLH ack token={self.token} error=0 busy=0 received=0\n')
        elif words[0] == 'hl.gateway':
            nonce = int(words[1])
            response = (f'NLH gateway abi=1 provider_registered=1\n'
                        f'NLH gateway source_sha256={"a" * 64}\n'
                        f'NLH ack nonce={nonce + 1} token=0 error=0 busy=0\n'
                        f'prefixNLH ack nonce={nonce} token=0 error=0 busy=0\n')
        self.incoming.extend(response.encode())

    def flush(self): pass
    def read(self, count):
        data = bytes(self.incoming[:count])
        del self.incoming[:count]
        if data: print('RX', data.decode().rstrip())
        return data


def record(name, run):
    print('@BEGIN', name)
    run()
    print('@RESULT', json.dumps({'id': name, 'status': 'pass'}))


def protocol():
    with tempfile.TemporaryDirectory() as directory:
        image = Path(directory) / 'test.nlh'
        image.write_bytes(pack_image(b'\0' * 4, 0, 16, 'test', 1, 1, 1, 'service', []))
        port = Port()
        client = Client(Wire(port, io.BytesIO(), 0.02))
        assert client.status()['provider'] == 1
        assert client.load(image)['active'] == 1
        assert client.query('list')[0]['generation'] == 1
        assert client.gateway()[1]['source_sha256'] == 'a' * 64
        assert sum(command.startswith('hl.commit') for command in port.commands) == 1


def unknown():
    with tempfile.TemporaryDirectory() as directory:
        image = Path(directory) / 'test.nlh'
        image.write_bytes(pack_image(b'\0' * 4, 0, 16, 'test', 1, 1, 1, 'service', []))
        port = Port(lost_commit=True)
        client = Client(Wire(port, io.BytesIO(), 0.005))
        try:
            client.load(image)
            raise AssertionError('Lost commit must report unknown')
        except UnknownOutcome as error:
            assert error.resolution['status']['busy'] == 0
            assert error.resolution['modules'][0]['name'] == 'test'
        mutating = [c.split()[0] for c in port.commands]
        assert mutating.count('hl.commit') == 1
        assert 'hl.abort' not in mutating
        assert mutating[-2:] == ['hl.status', 'hl.list']


def mutex():
    key = 'nlh-offline-test-' + uuid.uuid4().hex
    command = [sys.executable, str(Path(__file__).resolve()), '--probe-lock', key]
    with DeviceMutex(key):
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        print('locked child:', result.returncode, result.stdout.strip())
        assert result.returncode == 2
    result = subprocess.run(command, capture_output=True, text=True, timeout=5)
    print('released child:', result.returncode, result.stdout.strip())
    assert result.returncode == 0


class DiagnosticPort(Port):
    def __init__(self, incomplete=False, refused=False):
        super().__init__()
        self.incomplete, self.refused = incomplete, refused
        # A complete old record must be discarded by the readiness barrier.
        self.incoming.extend(b'GIULIA AUDIO: proto_fixture cases=20 passed_mask=0 changed=99\n')

    def write(self, raw):
        command = raw.decode().strip()
        if command not in ('audio.proto', 'modem.proto'):
            return super().write(raw)
        self.commands.append(command)
        print('TX', command)
        if self.refused:
            reply = 'consoleGIULIA AUDIO: proto_fixture rejected=debug-disabled synthetic=1 hardware_verified=0\n'
        elif command == 'audio.proto':
            reply = ('prefixGIULIA AUDIO: proto_fixture synthetic=1 hardware_verified=0 counter_exact=1 '
                     'cases=20 passed_mask=0xfffff expected_mask=0xfffff failed_mask=0 '
                     'epoch=1 attempted=16 adopted=16 fallback=0 rejected=0' +
                     ('' if self.incomplete else ' changed=0') + '\n')
        else:
            reply = ''.join('prefixGIULIA MODEM PROTO: source=fixture case=golden-' + str(i) +
                ' expected=0x0 actual=0x0 pass=1 module_delta=' + str(int(i < 20)) +
                ' fallback_delta=' + str(int(i >= 20)) + ' hardware_event=0\n' for i in range(22))
            reply += ('GIULIA MODEM PROTO: source=fixture cases=22 passed=22 failed=0 '
                      'module_delta=20 fallback_delta=2 native_delta=0 state_unchanged=1 hardware_event=0\n')
        self.incoming.extend(reply.encode())


def diagnostic_complete():
    port = DiagnosticPort()
    client = Client(Wire(port, io.BytesIO(), 0.02))
    audio = client.diagnostic('audio.proto')
    assert audio['scope'] == 'synthetic' and audio['hardware_verified'] is False
    assert audio['summary']['changed'] == 0 and len(audio['rows']) == 1
    modem = client.diagnostic('modem.proto')
    assert modem['summary']['module_delta'] == 20 and len(modem['rows']) == 23
    assert port.commands.count('audio.proto') == 1 and port.commands.count('modem.proto') == 1
    assert not any(command in ('audio.start', 'modem.start', 'wifi.start') for command in port.commands)


def diagnostic_timeout():
    port = DiagnosticPort(incomplete=True)
    client = Client(Wire(port, io.BytesIO(), 0.005))
    try:
        client.diagnostic('audio.proto')
        raise AssertionError('Incomplete fixture must not be accepted')
    except UnknownOutcome as error:
        assert error.resolution['diagnostic_rows'][0]['cases'] == 20
    assert port.commands.count('audio.proto') == 1
    assert [command.split()[0] for command in port.commands[-2:]] == ['hl.status', 'hl.list']


def diagnostic_refusal():
    port = DiagnosticPort(refused=True)
    client = Client(Wire(port, io.BytesIO(), 0.02))
    try:
        client.diagnostic('audio.start')
        raise AssertionError('Hardware actions are outside this diagnostic interface')
    except ValueError:
        assert not port.commands
    try:
        client.diagnostic('audio.proto')
        raise AssertionError('Refusal must remain blocked')
    except DiagnosticBlocked as error:
        assert error.response['hardware_verified'] is False


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--probe-lock':
        try:
            with DeviceMutex(sys.argv[2]):
                print('acquired')
        except TimeoutError:
            print('refused')
            raise SystemExit(2)
    else:
        record('transport-prefix-nonce-and-done-before-ack', protocol)
        record('unknown-commit-status-list-no-retry', unknown)
        record('cross-process-device-mutex', mutex)
        record('diagnostic-completion-and-old-record-barrier', diagnostic_complete)
        record('diagnostic-incomplete-status-list-no-retry', diagnostic_timeout)
        record('diagnostic-allowlist-and-refusal', diagnostic_refusal)
