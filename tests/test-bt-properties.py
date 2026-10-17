# SPDX-License-Identifier: GPL-3.0-only
# Licensed under the GNU General Public License, version 3 only.
# Provided without warranty. See LICENSE for the license text.
# Additional permission: BLUETOOTH-LINKING-EXCEPTION.txt.

from pathlib import Path
import re
import subprocess

from test_support import output_dir, write_bluetooth_text
from toolchain import VC, SDK, VERSION, INC, LIB

ROOT = Path(__file__).resolve().parents[1]
OUT = output_dir('bt-properties')
OUT.mkdir(parents=True, exist_ok=True)

HARNESS = (ROOT / 'tests/bt-properties.c').read_text(encoding='utf-8-sig')
inf = (ROOT / 'bt/inf/ap6256bt.inf').read_text(encoding='utf-8-sig')
defaults = []
for name in ('DebugLogLevel', 'InitialBaudRate', 'DownloadBaudRate', 'OperationalBaudRate',
             'PostLaunchDelayMs', 'RegOnDelayMs', 'ConfigureSleepMode', 'ConfigureAudio'):
    match = re.search(r'^HKR,(?:Parameters)?,' + name + r',0x[0-9a-fA-F]+,([0-9]+)\s*$', inf, re.M)
    assert match, name
    defaults.append('#define BT_DEFAULT_' + name + ' ' + match[1] + 'u')
write_bluetooth_text(OUT / 'bt-property-defaults.h', '\n'.join(defaults) + '\n')

write_bluetooth_text(OUT / 'properties-test.c', HARNESS)
includes = [ROOT / 'bt/props', ROOT / 'bt/src', VC / 'include', INC / 'ucrt', INC / 'shared', INC / 'um']
resource = OUT / 'properties.res'
subprocess.run([str(SDK / ('bin/' + VERSION + '/x64/rc.exe')), '/nologo',
                *[arg for path in includes for arg in ('/I', str(path))],
                '/fo', str(resource), str(ROOT / 'bt/props/properties.rc')], check=True, cwd=OUT)
subprocess.run([str(VC / 'bin/HostX64/x64/cl.exe'), '/nologo', '/W4', '/WX', '/DUNICODE', '/D_UNICODE',
                *['/I' + str(path) for path in includes], str(OUT / 'properties-test.c'), str(resource),
                '/Fe:' + str(OUT / 'properties-test.exe'), '/Fo:' + str(OUT / 'properties-test.obj'),
                '/link', '/LIBPATH:' + str(VC / 'lib/x64'), '/LIBPATH:' + str(LIB / 'ucrt/x64'),
                '/LIBPATH:' + str(LIB / 'um/x64'), 'user32.lib', 'comctl32.lib', 'setupapi.lib', 'advapi32.lib'],
               check=True, cwd=OUT)
subprocess.run([str(OUT / 'properties-test.exe')], check=True, cwd=OUT)
