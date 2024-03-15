# -*- coding: utf-8 -*-
import signal
import subprocess
import os

import pytest
import yatest.common as yc

gdbdir = os.path.dirname(os.path.dirname(yc.gdb_path()))
gdbpath = gdbdir + '/bin/bin_gdb/gdb'
gdbinit = yc.source_path('devtools/gdb/__init__.py')
gdbtest = yc.binary_path('devtools/gdb/test/tconts_stub/tconts_stub')

# Do not suspend with SIGTTOU when running in the background process group.
signal.signal(signal.SIGTTOU, signal.SIG_IGN)


def gdb(*commands):
    cmd = [gdbpath, '-nx', '-batch', '-x', gdbinit, '-ex', 'break BreakpointFunc()', '-ex', 'run']
    for c in commands:
        cmd += ['-ex', c]
    cmd += [gdbtest]
    env = os.environ
    env['PYTHONHOME'] = gdbdir
    env['PYTHONPATH'] = gdbdir + '/share/gdb/python'
    return subprocess.check_output(cmd, stderr=subprocess.STDOUT, env=env)


@pytest.mark.parametrize('bt', (' with-bt', ''), ids=('with-bt', 'without-bt'))
def test_print_conts(bt):
    out = gdb('print-conts' + bt)
    assert 'Found 2 cont(s)' in out and 'Cont #0: "ready coroutine", state: ready' in out and 'Cont #1: "waiting coroutine", state: waiting' in out
