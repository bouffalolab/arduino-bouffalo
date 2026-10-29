"""Minimal virtual-call ABI remains fatal and requires exceptions disabled."""
from pathlib import Path
import resource
import signal
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / './cores/bl616cl/cxx_fault.cpp'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory() as path:
    directory = Path(path)
    main = directory / 'main.cpp'
    main.write_text('''extern "C" void __cxa_pure_virtual();
extern "C" void __cxa_deleted_virtual();
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (argv[1][0] == '0') __cxa_pure_virtual();
    else __cxa_deleted_virtual();
    return 0;
}
''')
    executable = directory / 'test'
    command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-DBL616CL_MINIMAL_CXX_FAULT=1']
    subprocess.run(command + ['-fno-exceptions', str(source), str(main), '-o', str(executable)], check=True)
    for choice in ('0', '1'):
        result = subprocess.run([str(executable), choice], capture_output=True)
        assert result.returncode == -signal.SIGABRT, result.returncode
    result = subprocess.run(command + ['-c', str(source), '-o', str(directory / 'invalid.o')], capture_output=True)
    assert result.returncode != 0 and b'exceptions disabled' in result.stderr
    subprocess.run(['g++', '-c', str(source), '-o', str(directory / 'disabled.o')], check=True)
    result = subprocess.run(['nm', str(directory / 'disabled.o')], capture_output=True, text=True, check=True)
    assert '__cxa_pure_virtual' not in result.stdout and '__cxa_deleted_virtual' not in result.stdout
print('BL616CL_MINIMAL_CXX_FAULT_ABI_PASS')
