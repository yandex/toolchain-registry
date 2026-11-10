# Win32 OpenSSH full MinGW build

This directory contains the reproducible IX recipe used to build the complete
PowerShell/Win32 OpenSSH server and client payload for the Codenv Windows image.

The recipe builds `sshd.exe`, `sshd-session.exe`, `sshd-auth.exe`, the client
programs and SFTP helpers. Its Windows-specific fixes include:

- AF_UNIX client support and per-session Windows named pipes for forwarded
  authentication-agent connections;
- WinAPI-based executable, `ProgramData` and `PATH` initialization instead of
  incompatible MinGW CRT helpers;
- Windows service startup through the Service Control Dispatcher, with foreground fallback;
- handling `EAGAIN` while draining the nonblocking monitor log pipe;
- consistent `pid_t` handling in the ConPTY launcher, so an exited interactive
  shell is matched to its SSH session and `sshd-session` completes cleanup;
- replacement of the unavailable private UCRT API-set import with
  `ucrtbase.dll` for the target Windows image.

The ConPTY PID fix addresses interactive sessions that authenticated and ran
successfully but hung after `exit`. The server received `SIGCHLD` for the child
and logged the same decimal PID in the session dump, yet `session_by_pid`
reported it as unknown. The MinGW recipe uses the SDK's `pid_t`, while the
upstream launcher wrote the process ID through `int *`; using `pid_t *`
throughout preserves the full value and allows `session_close_by_pid` to send
the exit status and release the PTY.

The MinGW SDK defines a 64-bit `ssize_t`. The recipe gives the Win32 `read`,
`write` and `writev` wrappers matching return types, including their declarations,
so calls through the `atomicio` / `atomiciov` function pointers preserve negative
error results. Keeping the upstream 32-bit `int` return type would turn `-1` into
4294967295 and break SCP transfers when an I/O error needs to be retried.

The agent-forwarding transport addresses Windows images where server-side
AF_UNIX `bind` returns `EOPNOTSUPP`. Each forwarded session gets a unique
`\\.\pipe\openssh-agent-forward-*` byte-mode named pipe. Its DACL grants
access only to the session user, SYSTEM and local Administrators. The Win32 fd
layer exposes asynchronous `ConnectNamedPipe` completion through `select` and
`accept`, allowing the existing OpenSSH authentication-agent channel code to
remain unchanged. The ordinary Windows agent endpoint remains the standard
`\\.\pipe\openssh-ssh-agent` named pipe.

Validated build:

- Sandbox task: https://sandbox.yandex-team.ru/task/4747904129/view
- Sandbox resource: https://sandbox.yandex-team.ru/resource/13965899877/view
