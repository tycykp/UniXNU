"""Exercise the actual AArch64 builder and reject changing build inputs.

Linux/WSL: python3 tests/verify_build.py --kit . --out /path/to/NEW
All mutation is confined to generated fixture sources under the new output.
No port is opened. Counter fixtures do not establish hardware execution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
        separators=(',', ':')).encode()).hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Suite:
    def __init__(self, args):
        self.args = args
        self.results = []
        self.normal = None
        self.summary = {'schema': 'nlh-builder-validation-v1',
            'scope': 'actual-arm64-build-tooling', 'hardware_tested': False,
            'device_accessed': False, 'status': 'blocked', 'cases': self.results}

    def flush(self):
        statuses = [case['status'] for case in self.results]
        self.summary['status'] = ('fail' if 'fail' in statuses else
            'blocked' if not statuses or 'blocked' in statuses else 'pass')
        temporary = self.args.out / 'result.json.tmp'
        temporary.write_text(json.dumps(self.summary, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.args.out / 'result.json')

    def record(self, name, run):
        print('@BEGIN', name, flush=True)
        try:
            details = run()
            result = {'id': name, 'status': 'pass', **(details or {})}
        except Exception as error:
            result = {'id': name, 'status': 'fail',
                      'error_type': type(error).__name__, 'reason': str(error)}
        self.results.append(result)
        self.flush()
        print('@RESULT', json.dumps(result, sort_keys=True), flush=True)

    def command(self, source, include, output, compiler=None):
        return [sys.executable, str(self.args.kit / 'build-module.py'),
            '--source', str(source), '--include-dir', str(include),
            '--name', 'fixture-counter', '--version', '1', '--out', str(output),
            '--clang', str(compiler or self.args.clang), '--linker', str(self.args.linker)]

    def start(self, name, command):
        path = self.args.out / (name + '.log')
        log = path.open('xb')
        log.write(('ARGV ' + json.dumps(command) + '\n').encode())
        log.flush()
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        try:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                env=environment, start_new_session=True)
        except BaseException:
            log.close()
            raise
        return process, log, path

    def finish(self, running):
        process, log, path = running
        try:
            code = process.wait(timeout=self.args.timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            log.write(b'\nTIMEOUT; process group terminated, no automatic retry\n')
            raise TimeoutError('Builder timeout; inspect ' + str(path))
        finally:
            log.close()
        return code, path

    def run(self, name, command):
        return self.finish(self.start(name, command))

    def fixture(self, name):
        base = self.args.out / 'fixtures' / name
        source, include = base / 'source', base / 'include'
        source.mkdir(parents=True)
        include.mkdir()
        header = include / 'fixture_config.h'
        header.write_text('#define COUNTER_STEP 3\n', encoding='utf-8')
        (include / 'fixture_extra.h').write_text('#define EXTRA_VALUE 9\n', encoding='utf-8')
        counter = source / 'counter.c'
        counter.write_text('#include "fixture_config.h"\n' +
            (self.args.kit / 'examples/counter.c').read_text(encoding='utf-8'), encoding='utf-8')
        return base, counter, include, header

    def clean_publication(self, output, absent=False):
        if absent:
            require(not os.path.lexists(output), 'Failed build published an output directory')
        require(not output.with_name('.' + output.name + '.build-lock').exists(),
                'A completed build left its exclusive lock')
        require(not list(output.parent.glob('.' + output.name + '.stage-*')),
                'A completed build left staging directories')

    def verify(self, output, source, include):
        from nlh_format import parse_image, pack_image
        builder = runpy.run_path(str(self.args.kit / 'build-module.py'))
        report = json.loads((output / 'module.json').read_text(encoding='utf-8'))
        image_path = output / 'fixture-counter.nlh'
        metadata = parse_image(image_path.read_bytes())
        require(all(report.get(key) == value for key, value in metadata.items()),
                'Manifest image metadata mismatch')
        require(report['image_sha256'] == digest(image_path), 'Image SHA256 mismatch')
        require(report['source_sha256'] == digest(source), 'Primary source SHA256 mismatch')
        require(report['source_inputs_sha256'] == canonical(report['source_inputs']),
                'Dependency manifest digest mismatch')
        require(report['build_inputs_sha256'] == canonical(report['build_inputs']),
                'Build parameter digest mismatch')
        build = report['build_inputs']
        require(build['source_inputs'] == report['source_inputs'], 'Build identity lost source inputs')
        require(build['kit_inputs'] == report['kit_inputs'] == builder['kit_hashes'](),
                'Build tooling/ABI identity mismatch')
        roots = [('kit', self.args.kit), ('source', source.parent), ('include_0', include)]
        pre = builder['dependency_paths'](output / 'module-before.d')
        post = builder['dependency_paths'](output / 'module.d')
        require(pre == post and builder['input_hashes'](pre, roots) == report['source_inputs'],
                'Actual preflight/compile dependency closure mismatch')
        require({self.args.kit / 'native_hotload.h', source, include / 'fixture_config.h'} <= pre,
                'Explicit fixture/public include dependencies missing')
        flags = build['compile_flags']
        require(any(flags[i:i + 2] == ['-I', str(include)] for i in range(len(flags) - 1)),
                'Fixture include root was not explicitly passed to the compiler')
        for command_name in ('dependency_command', 'compile_command'):
            command = report[command_name]
            require(command[0] == build['compiler'] and command[1:1 + len(flags)] == flags,
                    'Preflight and compilation flags differ')
        require('-MM' in report['dependency_command'] and '-MMD' in report['compile_command'],
                'Actual compiler dependency modes missing')
        require(report['link_command'][0] == build['linker'] and
                report['link_command'][1:1 + len(build['link_flags'])] == build['link_flags'],
                'Recorded link parameters differ')
        expected = {'module.o', 'module.elf', 'module.d', 'module-before.d', image_path.name}
        require(set(report['artifacts']) == expected, 'Incomplete actual build artifact inventory')
        require(all(digest(output / name) == value for name, value in report['artifacts'].items()),
                'Actual build artifact digest mismatch')
        builder['check_object']((output / 'module.o').read_bytes())
        code, entry, state = builder['extract_elf']((output / 'module.elf').read_bytes())
        rebuilt = pack_image(code, entry, state, 'fixture-counter', 1, 1, 1, 'service', [])
        require(rebuilt == image_path.read_bytes(), 'Packaged bytes differ from actual AArch64 ELF')
        for tool, key in ((build['compiler'], 'compiler_identity'), (build['linker'], 'linker_identity')):
            actual = subprocess.check_output([tool, '--version'], text=True).strip()
            require(actual == report[key] == build[key], 'Tool identity differs from actual executable')
        require(report['atomic_output'] == 'verified-sibling-directory-rename-with-exclusive-build-lock',
                'Atomic publication identity missing')
        self.clean_publication(output)
        return {'image_sha256': digest(image_path), 'module_manifest_sha256': digest(output / 'module.json'),
                'dependencies': sorted(report['source_inputs']),
                'build_inputs_sha256': report['build_inputs_sha256']}

    def normal_case(self):
        base, source, include, header = self.fixture('normal')
        output = base / 'published'
        code, log = self.run('normal-counter', self.command(source, include, output))
        require(code == 0, 'Actual builder failed; inspect ' + str(log))
        evidence = self.verify(output, source, include)
        self.normal = (source, include, output)
        return {'raw_log': str(log), **evidence}

    def existing_case(self):
        require(self.normal is not None, 'Normal build prerequisite did not pass')
        source, include, output = self.normal
        before = {path.relative_to(output).as_posix(): digest(path)
                  for path in output.rglob('*') if path.is_file()}
        code, log = self.run('existing-output', self.command(source, include, output))
        after = {path.relative_to(output).as_posix(): digest(path)
                 for path in output.rglob('*') if path.is_file()}
        require(code != 0 and 'Output directory already exists' in log.read_text(errors='replace'),
                'Existing output was not explicitly refused')
        require(before == after, 'Existing output contents changed')
        self.clean_publication(output)
        return {'exit_code': code, 'raw_log': str(log), 'unchanged_files': before}

    def wrapper(self, base, header, output, behavior):
        path = base / 'compiler-wrapper'
        config = {'compiler': str(self.args.clang), 'header': str(header), 'output': str(output),
            'behavior': behavior, 'events': str(base / 'wrapper-events.jsonl'),
            'ready': str(base / 'ready'), 'release': str(base / 'release'),
            'timeout': self.args.timeout}
        script = '''#!/usr/bin/env python3
import hashlib, json, pathlib, subprocess, sys, time
CONFIG = __CONFIG__
args = sys.argv[1:]
def event(phase, **extra):
    output = pathlib.Path(CONFIG['output'])
    data = dict(phase=phase, output_exists=output.exists(),
        lock_exists=output.with_name('.' + output.name + '.build-lock').exists(), **extra)
    with pathlib.Path(CONFIG['events']).open('a', encoding='utf-8') as handle:
        handle.write(json.dumps(data) + '\\n')
def mutate(dependency=False):
    header = pathlib.Path(CONFIG['header'])
    header.write_text('#define COUNTER_STEP 17\\n' +
        ('#include "fixture_extra.h"\\n' if dependency else ''), encoding='utf-8')
    event('header-mutated', sha256=hashlib.sha256(header.read_bytes()).hexdigest())
if '-c' in args:
    event('compile-enter')
    if CONFIG['behavior'] in ('before-compile', 'dependency-set'):
        mutate(CONFIG['behavior'] == 'dependency-set')
    if CONFIG['behavior'] == 'hold':
        pathlib.Path(CONFIG['ready']).write_text('ready\\n')
        deadline = time.monotonic() + CONFIG['timeout']
        while not pathlib.Path(CONFIG['release']).exists():
            if time.monotonic() >= deadline:
                event('hold-timeout')
                sys.exit(88)
            time.sleep(0.005)
code = subprocess.call([CONFIG['compiler'], *args])
if '-MM' in args:
    event('preflight-finished', exit_code=code)
if '-c' in args:
    event('compile-finished', exit_code=code)
    if not code and CONFIG['behavior'] == 'after-compile': mutate()
    if not code and CONFIG['behavior'] == 'compiler-failure':
        event('injected-compiler-error')
        sys.exit(47)
sys.exit(code)
'''.replace('__CONFIG__', repr(config))
        path.write_text(script, encoding='utf-8')
        path.chmod(0o700)
        return path

    def rejection_case(self, name, behavior, message):
        base, source, include, header = self.fixture(name)
        output = base / 'published'
        compiler = self.wrapper(base, header, output, behavior)
        code, log = self.run(name, self.command(source, include, output, compiler))
        require(code != 0 and message in log.read_text(errors='replace'),
                'Actual builder did not reject the intended fault; inspect ' + str(log))
        events_path = base / 'wrapper-events.jsonl'
        events = [json.loads(line) for line in events_path.read_text().splitlines()]
        phases = [event['phase'] for event in events]
        require('preflight-finished' in phases and 'compile-finished' in phases,
                'The negative case did not invoke actual Clang preflight and compilation')
        require(all(not event['output_exists'] and event['lock_exists'] for event in events),
                'Partial output was visible or build lock was missing before publication')
        self.clean_publication(output, absent=True)
        return {'exit_code': code, 'raw_log': str(log), 'wrapper_events': events,
                'wrapper_events_path': str(events_path), 'successful_output_absent': True}

    def concurrency_case(self):
        base, source, include, header = self.fixture('atomic-publication')
        output = base / 'published'
        compiler = self.wrapper(base, header, output, 'hold')
        first = self.start('atomic-first-builder', self.command(source, include, output, compiler))
        completed = False
        try:
            deadline = time.monotonic() + self.args.timeout
            while not (base / 'ready').exists():
                require(first[0].poll() is None, 'First builder exited before compiler synchronization')
                require(time.monotonic() < deadline, 'Compiler did not reach synchronization point')
                time.sleep(0.005)
            require(not output.exists(), 'Output was visible before compilation finished')
            require(output.with_name('.' + output.name + '.build-lock').is_file(),
                    'First builder did not hold exclusive output lock')
            second_code, second_log = self.run('atomic-second-builder', self.command(source, include, output))
            require(second_code != 0 and 'FileExistsError' in second_log.read_text(errors='replace'),
                    'Concurrent builder did not refuse the held output lock')
            require(not output.exists(), 'Concurrent builder published partial output')
            require(output.with_name('.' + output.name + '.build-lock').is_file(),
                    'Refused concurrent builder removed the first builder lock')
            (base / 'release').write_text('release\n', encoding='utf-8')
            required_files = {'module.o', 'module.elf', 'module.d', 'module-before.d',
                              'fixture-counter.nlh', 'module.json'}
            deadline = time.monotonic() + self.args.timeout
            while first[0].poll() is None:
                if output.exists():
                    require(required_files <= {path.name for path in output.iterdir()},
                            'Partially published output directory was observable')
                require(time.monotonic() < deadline, 'First builder did not complete after release')
                time.sleep(0.001)
            first_code, first_log = self.finish(first)
            completed = True
            require(first_code == 0, 'First actual builder failed after release; inspect ' + str(first_log))
            evidence = self.verify(output, source, include)
            return {'raw_log': str(first_log), 'refused_builder_log': str(second_log),
                    'refused_builder_exit_code': second_code, 'partial_output_observed': False, **evidence}
        finally:
            if not completed:
                (base / 'release').write_text('release\n', encoding='utf-8')
                self.finish(first)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kit', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--linker', default='ld.lld')
    parser.add_argument('--timeout', type=float, default=45)
    args = parser.parse_args()
    args.kit, args.out = args.kit.resolve(), args.out.absolute()
    if not (args.kit / 'build-module.py').is_file(): parser.error('Kit builder not found')
    if not 0 < args.timeout <= 300: parser.error('Timeout must be in 0..300 seconds')
    args.out.mkdir(parents=True, exist_ok=False)
    suite = Suite(args)
    args.clang, args.linker = shutil.which(args.clang), shutil.which(args.linker)
    if os.name != 'posix' or not args.clang or not args.linker:
        suite.results.append({'id': 'toolchain-preconditions', 'status': 'blocked',
            'reason': 'Linux/WSL, actual clang and ld.lld are required; no device operation performed'})
        suite.flush()
        print(json.dumps(suite.summary, indent=2))
        return 2
    args.clang, args.linker = Path(args.clang).absolute(), Path(args.linker).absolute()
    sys.path.insert(0, str(args.kit))
    builder = runpy.run_path(str(args.kit / 'build-module.py'))
    before = builder['kit_hashes']()
    suite.summary['kit_inputs_before'] = before
    suite.record('normal-counter-provenance', suite.normal_case)
    suite.record('existing-output-preserved', suite.existing_case)
    suite.record('header-change-after-preflight', lambda: suite.rejection_case(
        'header-change-after-preflight', 'before-compile', 'source/header changed during compilation'))
    suite.record('header-change-after-compilation', lambda: suite.rejection_case(
        'header-change-after-compilation', 'after-compile', 'source/header changed during compilation'))
    suite.record('dependency-set-change', lambda: suite.rejection_case(
        'dependency-set-change', 'dependency-set', 'dependency set changed after preflight'))
    suite.record('compiler-error-does-not-publish', lambda: suite.rejection_case(
        'compiler-error-does-not-publish', 'compiler-failure', 'returned non-zero exit status 47'))
    suite.record('atomic-publication-and-output-lock', suite.concurrency_case)
    def unchanged():
        after = builder['kit_hashes']()
        suite.summary['kit_inputs_after'] = after
        require(before == after, 'Tests changed actual production kit tooling/ABI inputs')
        return {'production_kit_unchanged': True}
    suite.record('production-kit-unchanged', unchanged)
    print(json.dumps(suite.summary, indent=2, sort_keys=True))
    return {'pass': 0, 'fail': 1, 'blocked': 2}[suite.summary['status']]


if __name__ == '__main__':
    raise SystemExit(main())
