from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


root = Path.cwd()
out = Path(sys.argv[1])
sdk_paths = subprocess.run(
    ['cc', '-v', '-E', '-x', 'c', '/dev/null'],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True,
).stderr
sdk = None
for line in sdk_paths.splitlines():
    candidate = Path(line.strip())
    if (candidate / 'string.h').is_file() and (candidate / 'sys/types.h').is_file():
        if (candidate / 'windows.h').is_file():
            sdk = candidate
            break
if sdk is None:
    raise RuntimeError('MinGW SDK headers not found in compiler search path')


def replace(path, before, after):
    file = root / path
    value = file.read_text()
    if before not in value:
        raise RuntimeError('expected text missing in ' + path + ': ' + before)
    file.write_text(value.replace(before, after))


# The Windows fork ships a Visual Studio configuration and CRT header map.
config = (root / 'contrib/win32/openssh/config.h.vs').read_text()
config = config.replace('#define __attribute__(A)', '/* MinGW supports __attribute__ */')
(root / 'config.h').write_text(config)
crt_names = ['stdio.h', 'string.h', 'sys/types.h', 'ctype.h',
             'stdlib.h', 'sys/stat.h', 'fcntl.h', 'time.h']
crt_map = []
for name in crt_names:
    header = sdk / name
    if not header.is_file():
        raise RuntimeError('MinGW CRT header missing: ' + str(header))
    macro = name.upper().replace('/', '_').replace('.', '_')
    crt_map.append('#define ' + macro + ' "' + str(header) + '"')
(root / 'contrib/win32/win32compat/inc/crtheaders.h').write_text(
    '\n'.join(crt_map) + '\n')

# MSVC accepts backslashes and case-insensitive header names. Linux does not.
sdk_names = dict((p.name.lower(), p.name) for p in sdk.iterdir() if p.is_file())
sdk_names['debug.h'] = 'Debug.h'
include = re.compile(r'(?m)^(#\s*include\s*[<"])([^>"]+)([>"])')
for path in list(root.rglob('*.c')) + list(root.rglob('*.h')):
    if path.name == 'crtheaders.h':
        continue
    value = path.read_text(errors='surrogateescape')

    def convert(match):
        name = match.group(2).replace('\\', '/')
        name = sdk_names.get(name.lower(), name)
        return match.group(1) + name + match.group(3)

    fixed = include.sub(convert, value)
    if fixed != value:
        path.write_text(fixed, errors='surrogateescape')

types = 'contrib/win32/win32compat/inc/sys/types.h'
for declaration in ['typedef long long off_t;', 'typedef int ssize_t;',
                    'typedef int pid_t;']:
    replace(types, declaration, '')
stat = 'contrib/win32/win32compat/inc/sys/stat.h'
replace(stat, '#pragma once', '#pragma once\n#include <direct.h>')
replace(stat, '#include SYS_STAT_H', '#include SYS_STAT_H\nstruct w32_stat;')
fcntl = 'contrib/win32/win32compat/inc/fcntl.h'
replace(fcntl, '__VA_ARGS__', '##__VA_ARGS__')
replace('contrib/win32/win32compat/inc/sys/time.h',
        'int nanosleep(const struct timespec *, struct timespec *);', '')
unistd = root / 'contrib/win32/win32compat/inc/unistd.h'
unistd.write_text(unistd.read_text() + '\nint fork(void);\n')
select = 'contrib/win32/win32compat/inc/sys/select.h'
replace(select, 'int w32_select(', 'struct w32_timeval;\nint w32_select(')
w32fd = 'contrib/win32/win32compat/w32fd.c'
for name in ('read', 'write', 'writev'):
    replace(unistd, 'int w32_' + name + '(', 'ssize_t w32_' + name + '(')
    replace(w32fd, '\nint\nw32_' + name + '(', '\nssize_t\nw32_' + name + '(')
replace(w32fd, 'void fd_table_set(struct w32_io* pio, int index);',
        'static void fd_table_set(struct w32_io* pio, int index);')
replace(w32fd, 'void fd_decode_state(char*);',
        'static void fd_decode_state(char*);')
replace('contrib/win32/win32compat/signal.c',
        '#undef NSIG\n#include <signal.h>',
        '#undef NSIG\n#include "' + str(sdk / 'signal.h') + '"')
misc = 'contrib/win32/win32compat/misc.c'
replace(misc, '\nint\nnanosleep(', '\n#ifndef __MINGW32__\nint\nnanosleep(')
replace(misc, '\n/* This routine is contributed by',
        '\n#endif\n\n/* This routine is contributed by')
replace('contrib/win32/win32compat/ansiprsr.c',
        'TelParams Parameters;', 'extern TelParams Parameters;')

# The MinGW SDK owns pid_t after the duplicate Win32 compatibility typedef is
# removed above. Keep the ConPTY launcher interface in the same type: writing a
# DWORD through int * can leave the upper part of pid_t unchanged. Then the PID
# printed by session_by_pid looks correct, but it does not compare equal to the
# value returned by waitpid and the PTY session is never cleaned up.
replace('contrib/win32/win32compat/misc_internal.h',
        '#pragma once',
        '#pragma once\n#include <sys/types.h>')
replace('contrib/win32/win32compat/misc_internal.h',
        'int exec_command_with_pty(int * pid,',
        'int exec_command_with_pty(pid_t * pid,')
replace('contrib/win32/win32compat/win32_pty.c',
        'int exec_command_with_pty(int * pid,',
        'int exec_command_with_pty(pid_t * pid,')
replace('contrib/win32/win32compat/win32_pty.c',
        '*pid = pi.dwProcessId;',
        '*pid = (pid_t)pi.dwProcessId;')

replace('sshpty.h', '#include <termios.h>', '#include <termios.h>\n#include <pwd.h>')

replace('contrib/win32/win32compat/ssh-agent/agent.c', '&(ULONG_PTR)con', '(PULONG_PTR)&con')

replace('contrib/win32/win32compat/ssh-agent/connection.c', 'int process_request(struct agent_connection*);', 'static int process_request(struct agent_connection*);')

replace('contrib/win32/win32compat/ssh-agent/keyagent-request.c', 'sshbuf_ptr(request) - blob', 'sshbuf_ptr(request) - (const u_char *)blob')

replace('contrib/win32/win32compat/ssh-agent/agent.c', 'char* sshagent_con_username;', 'extern char* sshagent_con_username;')
replace('contrib/win32/win32compat/ssh-agent/agent.c', 'HANDLE sshagent_client_primary_token;', 'extern HANDLE sshagent_client_primary_token;')

# MinGW's wide CRT environment helpers fail before sshd configures logging and
# make wmain return -1 (observed as exit code 255). Use the process environment
# API directly and report startup errors to stderr.
wmain_sshd = root / 'contrib/win32/win32compat/wmain_sshd.c'
wmain_value = wmain_sshd.read_text()
path_begin = wmain_value.index('\t_wdupenv_s(&path_value, &len, L"PATH");')
path_end = wmain_value.index('\n\tif (!StartServiceCtrlDispatcherW(dispatch_table))', path_begin)
path_setup = '''\tDWORD path_len = GetEnvironmentVariableW(L"PATH", NULL, 0);
\tsize_t prog_len = wcslen(__wprogdir);
\tpath_new_len = prog_len + (path_len ? path_len : 1) + 2;
\tif ((path_new_value = calloc(path_new_len, sizeof(wchar_t))) == NULL) {
\t\tfwprintf(stderr, L"sshd: cannot allocate PATH buffer\\n");
\t\treturn -1;
\t}
\twcscpy_s(path_new_value, path_new_len, __wprogdir);
\tif (path_len) {
\t\twcscat_s(path_new_value, path_new_len, L";");
\t\tGetEnvironmentVariableW(L"PATH", path_new_value + prog_len + 1,
\t\t    (DWORD)(path_new_len - prog_len - 1));
\t}
\tif (!SetEnvironmentVariableW(L"PATH", path_new_value)) {
\t\tfwprintf(stderr, L"sshd: SetEnvironmentVariableW(PATH) failed: %lu\\n", GetLastError());
\t\tfree(path_new_value);
\t\treturn -1;
\t}
\tfree(path_new_value);
'''
wmain_value = wmain_value[:path_begin] + path_setup + wmain_value[path_end:]
wmain_value = wmain_value.replace(
    '\t\telse\n\t\t\treturn -1;\n',
    '\t\telse {\n\t\t\tfwprintf(stderr, L"sshd: StartServiceCtrlDispatcherW failed: %lu\\n", GetLastError());\n\t\t\treturn -1;\n\t\t}\n')
wmain_sshd.write_text(wmain_value)
for auxiliary in ('wmain_sshd-session.c', 'wmain_sshd-auth.c'):
    auxiliary_path = root / 'contrib/win32/win32compat' / auxiliary
    auxiliary_value = auxiliary_path.read_text()
    auxiliary_begin = auxiliary_value.index('\t_wdupenv_s(&path_value, &len, L"PATH");')
    auxiliary_end = auxiliary_value.index('\n\treturn ', auxiliary_begin)
    auxiliary_value = auxiliary_value[:auxiliary_begin] + path_setup + auxiliary_value[auxiliary_end:]
    auxiliary_path.write_text(auxiliary_value)
w32fd = root / 'contrib/win32/win32compat/w32fd.c'
w32fd_value = w32fd.read_text()
paths_begin = w32fd_value.index('void \ninit_prog_paths()')
paths_end = w32fd_value.index('\nvoid\nw32posix_initialize()', paths_begin)
paths_impl = '''void
init_prog_paths()
{
\twchar_t module_path[PATH_MAX];
\twchar_t program_data[PATH_MAX];
\tstatic int processed = 0;
\tif (processed)
\t\treturn;
\tif (GetModuleFileNameW(NULL, module_path, PATH_MAX) == 0)
\t\texit(255);
\t__wprogdir = _wcsdup(module_path);
\t__progdir = utf16_to_utf8(module_path);
\tif (__wprogdir == NULL || __progdir == NULL)
\t\texit(255);
\t__progname = strrchr(__progdir, '\\\\') + 1;
\t*strrchr(__progdir, '\\\\') = '\\0';
\t*wcsrchr(__wprogdir, L'\\\\') = L'\\0';
\t*(__progname + strlen(__progname) - 4) = '\\0';
\tif (GetEnvironmentVariableW(L"ProgramData", program_data, PATH_MAX) == 0)
\t\twcscpy_s(program_data, PATH_MAX, L"C:\\\\ProgramData");
\t__wprogdata = _wcsdup(program_data);
\t__progdata = utf16_to_utf8(program_data);
\tif (__wprogdata == NULL || __progdata == NULL)
\t\texit(255);
\tprocessed = 1;
}
'''
w32fd.write_text(w32fd_value[:paths_begin] + paths_impl + w32fd_value[paths_end:])
monitor = root / 'monitor.c'
monitor_value = monitor.read_text()
monitor_value = monitor_value.replace(
    '''\tif (atomicio(read, pmonitor->m_log_recvfd, p, 4) != 4) {
\t\tif (errno == EPIPE) {''',
    '''\tif (atomicio(read, pmonitor->m_log_recvfd, p, 4) != 4) {
\t\tif (errno == EAGAIN || errno == EWOULDBLOCK) {
\t\t\tsshbuf_free(logmsg);
\t\t\treturn 1;
\t\t}
\t\tif (errno == EPIPE) {''', 1)
monitor.write_text(monitor_value)

objects = out / 'objects'
objects.mkdir(parents=True, exist_ok=True)
flags = ['-I.', '-Iincludes', '-Icontrib/win32/win32compat/inc',
         '-Icontrib/win32/win32compat', '-Ilibkrb', '-Ilibkrb/libKrb5',
         '-Icontrib/win32/openssh', '-Iopenbsd-compat',
         '-Dmain=openssh_main', '-DWIN32', '-D_WIN32_WINNT=0x601', '-D_CRT_SECURE_NO_WARNINGS',
         '-D_CRT_NONSTDC_NO_WARNINGS', '-D_WINSOCK_DEPRECATED_NO_WARNINGS',
         '-DHAVE_SSIZE_T=1', '-DHAVE_CLOCK_GETTIME=1',
         '-DHAVE_EVP_DIGESTSIGN=1', '-DHAVE_EVP_DIGESTVERIFY=1',
         '-include', str(root / 'mingw_compat.h'),
         '-Wno-implicit-function-declaration',
         '-Wno-incompatible-function-pointer-types', '-w']


failures = []

def run(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode:
        sys.stderr.write('command: ' + ' '.join(map(str, command)) + '\n')
        sys.stderr.write(result.stdout + result.stderr)
        failures.append(command)


programs = ['ssh', 'sshd', 'sshd-auth', 'sshd-session', 'scp', 'sftp', 'sftp-server', 'ssh-add', 'ssh-agent', 'keygen', 'ssh-keyscan', 'ssh-pkcs11-helper', 'ssh-shellhost']
libraries = ['libssh', 'openbsd_compat', 'win32iocompat']
projects = programs + libraries
members = dict((name, []) for name in projects)
compiled = set()
project_dir = root / 'contrib/win32/openssh'
for project in projects:
    xml = ET.parse(project_dir / (project + '.vcxproj'))
    for entry in xml.findall('.//{*}ClCompile'):
        source = entry.get('Include')
        if not source or source.endswith('sshTelemetry.c'):
            continue
        source = source.replace('$(OpenSSH-Src-Path)', '').replace('\\', '/')
        if source.startswith('../'):
            source = (project_dir / source).resolve().relative_to(root).as_posix()
        source = source.lstrip('/')
        if not (root / source).is_file():
            source = 'contrib/win32/openssh/' + source
        if not (root / source).is_file():
            raise RuntimeError('source missing: ' + source)
        obj = objects / (source.replace('/', '_') + '.o')
        if source not in compiled:
            command = ['cc', '-c', source, '-o', str(obj)] + flags
            if not source.endswith('/termio.c'):
                command.append('-fgnu89-inline')
            run(command)
            compiled.add(source)
        if obj not in members[project]:
            members[project].append(obj)

if failures:
    raise SystemExit('compilation failed for ' + str(len(failures)) + ' sources')

telemetry = objects / 'telemetry_stub.o'
run(['cc', '-c', 'telemetry_stub.c', '-o', str(telemetry),
     '-Icontrib/win32/openssh'])
for program in programs:
    members[program].append(telemetry)
startup = root / 'mingw_startup.c'
startup.write_text('#include <windows.h>\n#include <shellapi.h>\n\nint wmain(int argc, wchar_t **argv);\n\nint main(void)\n{\n    int argc;\n    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);\n    if (argv == NULL)\n        return 1;\n    int result = wmain(argc, argv);\n    LocalFree(argv);\n    return result;\n}\n')
startup_object = objects / 'mingw_startup.o'
run(['cc', '-c', str(startup), '-o', str(startup_object)])

compat = objects / 'link_compat.o'
run(['cc', '-c', 'link_compat.c', '-o', str(compat)])

archives = []
for project in libraries:
    archive = objects / ('lib' + project + '.a')
    run(['ar', 'rcs', str(archive)] + list(map(str, members[project])))
    archives.append(archive)

(out / 'bin').mkdir(parents=True, exist_ok=True)
for program in programs:
    name = 'ssh-keygen' if program == 'keygen' else program
    link = ['cc', '-o', str(out / ('bin/' + name + '.exe'))]
    link += list(map(str, members[program])) + [str(startup_object), str(compat), '-Wl,--start-group']
    link += list(map(str, [archives[2], archives[0], archives[1]]))
    link += ['-lcrypto', '-lz', '-lws2_32', '-lcrypt32', '-lbcrypt',
             '-luser32', '-ladvapi32', '-lsecur32', '-lshlwapi', '-lnetapi32',
             '-lrpcrt4', '-lole32', '-loleaut32', '-luserenv', '-lwtsapi32',
             '-lsetupapi', '-lhid', '-lshell32', '-Wl,--end-group']
    run(link)
    executable = out / ('bin/' + name + '.exe')
    if executable.is_file():
        image = executable.read_bytes()
        private_ucrt = b'api-ms-win-crt-private-l1-1-0.dll\0'
        system_ucrt = b'ucrtbase.dll\0'.ljust(len(private_ucrt), b'\0')
        if image.count(private_ucrt) != 1:
            raise RuntimeError('unexpected private UCRT import count in ' + str(executable))
        executable.write_bytes(image.replace(private_ucrt, system_ucrt))
    print(executable, flush=True)

if failures:
    raise SystemExit('link failed for ' + str(len(failures)) + ' commands')
