# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.

from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    files = []
    for folder in ('src', 'inf', 'bt', 'fn', 'tests'):
        files += [path for path in (ROOT / folder).rglob('*') if path.is_file()
                  and '__pycache__' not in path.parts]
    files += [ROOT / name for name in ('LICENSE', 'LICENSE-ISC.txt', 'NOTICE.txt', 'BLUETOOTH-LINKING-EXCEPTION.txt')]
    return {path.relative_to(ROOT).as_posix(): sha(path) for path in sorted(files)}


def main():
    parser = argparse.ArgumentParser(description='Run host regressions against the driver source.')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--suite', action='append', help='Run one named suite; repeat to select several.')
    parser.add_argument('--include-optional', action='store_true', help='Include any suites marked optional in the manifest.')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    manifest = json.loads((ROOT / 'tests/suites.json').read_text(encoding='utf-8'))
    known = {suite['name']: suite for suite in manifest['suites']}
    if args.list:
        for name, suite in known.items():
            print(name + (' (optional)' if suite['optional'] else ''))
        return
    if not args.output:
        parser.error('--output is required; choose a new directory outside the checkout')
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error('--output must be outside the checkout')
    if output.exists() and any(output.iterdir()):
        parser.error('--output must be empty to prevent stale test results')
    if args.timeout < 1:
        parser.error('--timeout must be positive')
    if args.suite:
        unknown = set(args.suite) - known.keys()
        if unknown:
            parser.error('Unknown suites: ' + ', '.join(sorted(unknown)))
        suites = [known[name] for name in dict.fromkeys(args.suite)]
    else:
        suites = [suite for suite in known.values() if args.include_optional or not suite['optional']]
    if any(suite['optional'] for suite in suites) and not args.include_optional:
        parser.error('The selected optional suite requires --include-optional')
    import toolchain
    import cryptography
    tools = toolchain.describe()
    tools['cryptography_version'] = cryptography.__version__
    tools['python_version'] = sys.version
    tools['compiler_sha256'] = sha(Path(tools['compiler']))
    tools['codec_sha256'] = sha(toolchain.LIB / 'um/x64/wlan/2.0/WificxTLVGenParse.lib')
    output.mkdir(parents=True, exist_ok=True)
    original = inputs()
    (output / 'inputs.json').write_text(json.dumps(original, indent=2) + '\n', encoding='utf-8')
    result = dict(schema=1, passed=False, board_verified=False, tools=tools, suites=[],
                  inputs_sha256=sha(output / 'inputs.json'))
    failures = 0
    for suite in suites:
        folder = output / suite['name']
        folder.mkdir()
        environment = dict(os.environ)
        environment['AP6256_TEST_OUTPUT'] = str(folder)
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        environment['PYTHONUTF8'] = '1'
        command = [sys.executable, '-B', str(ROOT / 'tests/run_one.py'), str(ROOT / 'tests' / suite['file'])]
        command += suite['flags']
        if suite['output']:
            command += [suite['output'], str(folder / 'work')]
        print('RUN ' + suite['name'], flush=True)
        started = time.monotonic()
        with (folder / 'suite.log').open('wb') as log:
            try:
                child = subprocess.Popen(command, cwd=folder, env=environment, stdout=log, stderr=subprocess.STDOUT)
                exit_code = child.wait(timeout=args.timeout)
                error = None
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], stdout=log, stderr=subprocess.STDOUT)
                child.wait()
                exit_code = None
                error = 'Suite exceeded its timeout.'
        passed = exit_code == 0
        failures += not passed
        result['suites'].append(dict(name=suite['name'], passed=passed, exit_code=exit_code,
                                     elapsed_seconds=round(time.monotonic() - started, 3), error=error,
                                     log=suite['name'] + '/suite.log', log_sha256=sha(folder / 'suite.log')))
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(('PASS ' if passed else 'FAIL ') + suite['name'], flush=True)
        if not passed:
            print((folder / 'suite.log').read_text(encoding='utf-8', errors='replace')[-3000:], flush=True)
    changed = [name for name, digest in original.items() if not (ROOT / name).is_file() or sha(ROOT / name) != digest]
    result.update(passed=failures == 0 and not changed, changed_inputs=changed, failed_suites=failures)
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'{len(suites) - failures}/{len(suites)} suites passed', flush=True)
    raise SystemExit(0 if result['passed'] else 1)


if __name__ == '__main__':
    main()
