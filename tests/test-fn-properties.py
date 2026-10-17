# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.

from pathlib import Path
import os
import subprocess

from test_support import output_dir, write_text
from toolchain import VC, SDK, VERSION, INC, LIB

ROOT = Path(__file__).resolve().parents[1]
OUT = output_dir('fn-properties')
OUT.mkdir(parents=True, exist_ok=True)
write_text(OUT / 'fn-properties-test.c', (ROOT / 'tests/fn-properties.c').read_text(encoding='utf-8-sig'))
includes = [ROOT / 'fn/props', ROOT / 'fn/src', VC / 'include', INC / 'ucrt', INC / 'shared', INC / 'um']
resource = OUT / 'properties.res'
subprocess.run([str(SDK / ('bin/' + VERSION + '/x64/rc.exe')), '/nologo',
                *[arg for path in includes for arg in ('/I', str(path))],
                '/fo', str(resource), str(ROOT / 'fn/props/properties.rc')], check=True, cwd=OUT)
subprocess.run([str(VC / 'bin/HostX64/x64/cl.exe'), '/nologo', '/W4', '/WX', '/DUNICODE', '/D_UNICODE',
                *['/I' + str(path) for path in includes], str(OUT / 'fn-properties-test.c'), str(resource),
                '/Fe:' + str(OUT / 'fn-properties-test.exe'), '/Fo:' + str(OUT / 'fn-properties-test.obj'),
                '/link', '/LIBPATH:' + str(VC / 'lib/x64'), '/LIBPATH:' + str(LIB / 'ucrt/x64'),
                '/LIBPATH:' + str(LIB / 'um/x64'), 'user32.lib', 'comctl32.lib', 'setupapi.lib', 'advapi32.lib'], check=True, cwd=OUT)
subprocess.run([str(OUT / 'fn-properties-test.exe')], check=True, cwd=OUT)
for component in ('bt', 'fn'):
    project = (ROOT / component / 'props' / ('ap6256' + component + 'props.vcxproj')).read_text()
    assert '<RuntimeLibrary>MultiThreaded</RuntimeLibrary>' in project
assert 'ap6256fnprops.dll,Ap6256FnPropPageProvider' in (ROOT / 'fn/inf/ap6256fn.inf').read_text()
powershell = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
for invalid in ('1.0', '1.0.0.65536', '65536.0.0.0'):
    result = subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-File', str(ROOT / 'build-source.ps1'),
                             '-Version', invalid, '-OutputDirectory', str(OUT / 'invalid-version')],
                            capture_output=True, text=True, cwd=OUT)
    assert result.returncode != 0 and 'Version must contain four components' in result.stdout + result.stderr
assert not (OUT / 'invalid-version').exists()
