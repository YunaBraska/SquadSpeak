"""Bound a native test and preserve Windows thread stacks before killing a hang."""
import argparse
import math
import os
from pathlib import Path
import subprocess
import sys


def write_dump(pid, path):
    import ctypes
    from ctypes import wintypes
    import msvcrt

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    dbghelp.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                        wintypes.HANDLE, wintypes.DWORD,
                                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    dbghelp.MiniDumpWriteDump.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x0410, False, pid)  # query information and read memory
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as output:
            # Include thread information and unloaded modules, not full process memory.
            if not dbghelp.MiniDumpWriteDump(handle, pid, msvcrt.get_osfhandle(output.fileno()),
                                            0x1020, None, None, None):
                raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--dump", type=Path, required=True)
    parser.add_argument("--dump-process", type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.dump_process is not None:
        write_dump(args.dump_process, args.dump)
        return 0
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not args.command:
        parser.error("a positive timeout and a command are required")
    # A failed launch must not leave an earlier run's dump looking like new evidence.
    args.dump.unlink(missing_ok=True)
    try:
        with subprocess.Popen(args.command, stdin=subprocess.DEVNULL) as child:
            try:
                return child.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                print(f"Test exceeded {args.timeout:g} seconds, process {child.pid}", file=sys.stderr, flush=True)
                if os.name == "nt":
                    try:
                        subprocess.run([sys.executable, __file__, "--dump-process", str(child.pid),
                                        "--dump", str(args.dump)], check=True, timeout=10)
                    except (OSError, subprocess.SubprocessError) as error:
                        print(f"Could not capture thread dump: {error}", file=sys.stderr, flush=True)
                return 124
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
    except OSError as error:
        print(f"Could not start test: {error}", file=sys.stderr)
        return 127


if __name__ == "__main__":
    sys.exit(main())
