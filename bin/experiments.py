#!/usr/bin/env python
import sys
import os
from contextlib import contextmanager
import imp
import dumptruck
import functools
import time
import subprocess
import threading

APPS = ['streamcluster']
APPSDIR = 'apps'
EVALSCRIPT = 'eval.py'
CONFIGFILE = 'accept_config.txt'
TIMEOUT_FACTOR = 3

dumptruck.PYTHON_SQLITE_TYPE_MAP[tuple] = u'json text'

@contextmanager
def chdir(d):
    """A context manager that temporary changes the working directory.
    """
    olddir = os.getcwd()
    os.chdir(d)
    yield
    os.chdir(olddir)

class Memoized(object):
    """Wrap a function and memoize it in a persistent database. Only
    positional arguments, not keyword arguments, are used as memo table
    keys. These (and the return value) must be serializable.
    """
    def __init__(self, dbname, func):
        self.func = func
        self.dt = dumptruck.DumpTruck(dbname)
        self.table = 'memo_{}'.format(func.__name__)

    def __call__(self, *args, **kwargs):
        if self.table in self.dt.tables():
            memoized = self.dt.execute(
                'SELECT res FROM {} WHERE args=? LIMIT 1'.format(self.table),
                (args,)
            )
            if memoized:
                return memoized[0]['res']

        res = self.func(*args, **kwargs)
        self.dt.insert(
            {'args': args, 'res': res},
            self.table
        )
        return res

memoize = functools.partial(Memoized, 'memo.db')

class CommandThread(threading.Thread):
    def __init__(self, command):
        super(CommandThread, self).__init__()
        self.proc = subprocess.Popen(command)

    def run(self):
        self.proc.wait()

def run_cmd(command, timeout=None):
    """Run a process with an optional timeout. Return a boolean
    indicating whether the process timed out.
    """
    thread = CommandThread(command)
    thread.start()
    thread.join(timeout)
    if thread.is_alive():
        thread.proc.terminate()
        return True
    return False

def execute(timeout):
    """Run the application in the working directory and return the
    wall-clock duration (in seconds) of the execution. If the command
    exceeds its timeout, return None instead.
    """
    start_time = time.time()
    timed_out = run_cmd(['make', 'run'], timeout)
    end_time = time.time()
    return None if timed_out else end_time - start_time

def build(approx=False):
    """Compile the application in the working directory. If `approx`,
    then it is built with ACCEPT relaxation enabled.
    """
    subprocess.check_call(['make', 'clean'])
    build_cmd = ['make', 'build']
    if approx:
        build_cmd.append('CLANGARGS=-mllvm -accept-relax')
    subprocess.check_call(build_cmd)

def parse_relax_config(f):
    """Parse a relaxation configuration from a file-like object.
    Generates (kind, ident, param) tuples.
    """
    for line in f:
        line = line.strip()
        if line:
            mod, ident, param = line.split(None, 2)
            yield mod, int(ident), int(param)

def dump_relax_config(config, f):
    """Write a relaxation configuration to a file-like object. The
    configuration should be a sequence of tuples.
    """
    for site in config:
        f.write('{} {} {}\n'.format(*site))

@memoize
def build_and_execute(appname, relax_config, timeout=None, loadfunc=None):
    """Build an application, run it, and collect its output. Return the
    parsed output, the duration of the execution, and the relaxation
    configuration.
    """
    if relax_config:
        with open(CONFIGFILE, 'w') as f:
            dump_relax_config(relax_config, f)
    elif os.path.exists(CONFIGFILE):
        os.remove(CONFIGFILE)

    build(bool(relax_config))
    elapsed = execute(timeout)
    if elapsed is None:
        # Timeout.
        output = None
    else:
        output = loadfunc()

    if not relax_config:
        with open(CONFIGFILE) as f:
            relax_config = list(parse_relax_config(f))

    return output, elapsed, relax_config

def permute_config(base):
    """Given a base (null) relaxation configuration, generate new
    (relaxed) configurations.
    """
    for i in range(len(base)):
        config = list(base)
        site = config[i]
        site = site[0], site[1], 1
        config[i] = site
        yield config

def evaluate(appname):
    with chdir(os.path.join(APPSDIR, appname)):
        mod = imp.load_source('evalscript', EVALSCRIPT)
        pout, ptime, base_config = build_and_execute(
            appname, None,
            timeout=None, loadfunc=mod.load
        )
        for config in permute_config(base_config):
            print(config)
            aout, atime, _ = build_and_execute(
                appname, config,
                timeout=ptime * TIMEOUT_FACTOR, loadfunc=mod.load
            )
            if atime is None:
                print('timed out')
            else:
                error = mod.score(pout, aout)
                print('{:.1%} error'.format(error))
                print('{:.2f} vs. {:.2f}: {:.2f}x'.format(
                    ptime, atime, ptime / atime
                ))

def experiments(appnames):
    for appname in appnames:
        evaluate(appname)

if __name__ == '__main__':
    experiments(sys.argv[1:] or APPS)
