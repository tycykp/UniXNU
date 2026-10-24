"""Build scheduler, VM and VFS core-policy replacement scenarios on Linux/WSL.

Produces baseline, changed-policy upgrade and lifecycle-failure images for
each reserved core name. This builds artifacts only; it never contacts a phone.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from nlh_format import CORE_INTERFACE, CORE_MODULES, CORE_STATE_ABI, parse_image

ROOT = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True, help='New suite directory')
    p.add_argument('--base-version', type=int, default=1,
                   help='Baseline version; upgrade uses +1, failure images use +2')
    p.add_argument('--upgrade-divisor', type=int, choices=range(2, 17), default=2)
    p.add_argument('--clang', default='clang')
    p.add_argument('--linker', default='ld.lld')
    a = p.parse_args()
    if not 1 <= a.base_version <= 0xfffffffd:
        p.error('Base version must leave room for two higher versions')
    inputs = ('build-core.py', 'build-module.py', 'module.ld', 'nlh_format.py',
              'native_hotload.h', 'native_hotload_core.h',
              'core/policy.c', 'core/native_core_algorithms.h',
              'core/LICENSE', 'core/UPSTREAM.md')
    if any(not (ROOT / name).is_file() for name in inputs):
        p.error('Incomplete core-policy kit')
    variants = {
        'baseline': {'version': a.base_version, 'divisor': 1},
        'upgrade': {'version': a.base_version + 1, 'divisor': a.upgrade_divisor},
        'fail-start': {'version': a.base_version + 2, 'divisor': a.upgrade_divisor,
                       'define': 'NLH_FAIL_START=1'},
        'fail-import': {'version': a.base_version + 2, 'divisor': a.upgrade_divisor,
                        'define': 'NLH_FAIL_IMPORT=1'},
        'fail-stop': {'version': a.base_version + 2, 'divisor': a.upgrade_divisor,
                      'define': 'NLH_FAIL_STOP=1'},
    }
    manifest = {
        'schema': 1, 'abi': 2, 'interface_abi': CORE_INTERFACE,
        'state_abi': CORE_STATE_ABI, 'kernel_pin': 'xnu-8796.141.3',
        'scope': 'scheduler quantum, VM sequential-access transition and VFS cluster I/O policies',
        'resident_core_objects': True, 'hardware_tested': False,
        'inputs': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
                   for name in inputs},
        'domains': CORE_MODULES, 'variants': variants, 'artifacts': [], 'scenarios': [],
        'failure_scenario_order': ['fail-start', 'fail-import', 'fail-stop'],
        'failure_scenario_notes': (
            'Load baseline then upgrade first. Failed START/IMPORT keep the upgrade active, '
            'so their unpublished version can be reused. Load fail-stop last: its START '
            'succeeds, then unloading it fails and quarantines the slot. Reboot the development '
            'kernel before repeating that scenario; equal or lower versions cannot replace it.'),
    }
    a.out.mkdir(parents=True, exist_ok=False)
    for name, domain in CORE_MODULES.items():
        paths = {}
        for variant, settings in variants.items():
            target = a.out / name / variant
            defines = [f'NLH_POLICY_DOMAIN={domain}',
                       f'NLH_POLICY_DIVISOR={settings["divisor"]}']
            if 'define' in settings:
                defines.append(settings['define'])
            command = [sys.executable, str(ROOT / 'build-module.py'),
                       '--source', str(ROOT / 'core' / 'policy.c'), '--out', str(target),
                       '--name', name, '--kind', 'core', '--version', str(settings['version']),
                       '--interface-abi', hex(CORE_INTERFACE), '--state-abi', str(CORE_STATE_ABI),
                       '--clang', a.clang, '--linker', a.linker]
            for definition in defines:
                command.extend(('--define', definition))
            subprocess.run(command, check=True)
            image_path = target / (name + '.nlh')
            image = image_path.read_bytes()
            report = parse_image(image)
            if report['core_domain'] != domain or report['version'] != settings['version']:
                raise ValueError('Built core image metadata differs from the requested scenario')
            relative = image_path.relative_to(a.out).as_posix()
            paths[variant] = relative
            manifest['artifacts'].append({
                **report, 'variant': variant, 'image': relative,
                'module_manifest': (target / 'module.json').relative_to(a.out).as_posix(),
                'image_sha256': hashlib.sha256(image).hexdigest(),
                'defines': defines,
            })
        running = {'expected_state': 'running', 'expected_active_version': a.base_version + 1}
        manifest['scenarios'].append({
            'name': name, 'domain': domain,
            'steps': [
                {'command': 'load', 'image': paths['baseline'], 'expected_error': 0,
                 'expected_state': 'running', 'expected_active_version': a.base_version},
                {'command': 'core', 'expect': 'domain policy calls rise under the corresponding workload'},
                {'command': 'load', 'image': paths['upgrade'], 'expected_error': 0, **running},
                {'command': 'call', 'name': name, 'value': 2,
                 'expect': 'imports >= 1; call/change counters preserved across the upgrade'},
                {'command': 'load', 'image': paths['fail-start'], 'expected_error': 5, **running},
                {'command': 'load', 'image': paths['fail-import'], 'expected_error': 5, **running},
                {'command': 'load', 'image': paths['fail-stop'], 'expected_error': 0,
                 'expected_state': 'running', 'expected_active_version': a.base_version + 2},
                {'command': 'unload', 'name': name, 'expected_error': 5,
                 'expected_state': 'quarantined', 'expected_active_version': a.base_version + 2},
                {'command': 'core', 'expect': 'quarantined domain uses the resident baseline'},
            ],
        })
    target = a.out / 'core-suite.json'
    target.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'{target}: {len(manifest["artifacts"])} core-policy images; hardware_tested=false')


if __name__ == '__main__':
    main()
