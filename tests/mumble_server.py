#!/usr/bin/env python3
"""Run the client E2E suite against an isolated, real Mumble server."""
import argparse
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import uuid

IMAGE = "mumblevoip/mumble-server@sha256:2373aaf071dfcea95b0d45bc2ddf6c10318674a313a7e4816e826e9ba4bca942"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    password = uuid.uuid4().hex
    owner = "squadspeak-mumble-" + uuid.uuid4().hex[:12]
    native = shutil.which("mumble-server") or shutil.which("murmurd")
    server = None
    container = False
    with tempfile.TemporaryDirectory(prefix="squadspeak-mumble-") as directory:
        root = Path(directory)
        with socket.socket() as tcp, socket.socket(type=socket.SOCK_DGRAM) as udp:
            tcp.bind(("127.0.0.1", 0))
            port = tcp.getsockname()[1]
            udp.bind(("127.0.0.1", port))
        log = (root / "server.log").open("w+")
        try:
            if native:
                config = root / "mumble.ini"
                # The CI container runs as root. The distro daemon otherwise
                # drops to its service user, which cannot enter this private directory.
                user = "uname=root\n" if hasattr(os, "geteuid") and os.geteuid() == 0 else ""
                config.write_text(f"database={root / 'mumble.sqlite'}\nport={port}\nhost=127.0.0.1\n"
                                  f"serverpassword={password}\nregisterName=SquadSpeak E2E\n"
                                  "defaultchannel=1\nbandwidth=1000000\nusers=128\nmessageburst=128\nautobanAttempts=0\nallowhtml=true\nlogfile=\npidfile=\n" + user)
                subprocess.run([native, "-ini", str(config), "-supw", password], check=True,
                               stdout=log, stderr=subprocess.STDOUT, timeout=20)
                server = subprocess.Popen([native, "-fg", "-ini", str(config)], stdout=log, stderr=subprocess.STDOUT)
            else:
                if not shutil.which("docker"):
                    raise SystemExit("Install mumble-server or start Docker to run the Mumble E2E suite.")
                container = True
                subprocess.run(["docker", "run", "--detach", "--name", owner,
                                "--publish", f"127.0.0.1:{port}:64738/tcp",
                                "--publish", f"127.0.0.1:{port}:64738/udp",
                                "--env", "MUMBLE_SUPERUSER_PASSWORD=" + password,
                                "--env", "MUMBLE_CONFIG_SERVER_PASSWORD=" + password,
                                "--env", "MUMBLE_CONFIG_REGISTER_NAME=SquadSpeak E2E",
                                "--env", "MUMBLE_CONFIG_BANDWIDTH=1000000",
                                "--env", "MUMBLE_CONFIG_DEFAULTCHANNEL=1",
                                "--env", "MUMBLE_CONFIG_ALLOW_HTML=true",
                                "--env", "MUMBLE_CONFIG_MESSAGEBURST=128",
                                "--env", "MUMBLE_CONFIG_AUTOBAN_ATTEMPTS=0", IMAGE], check=True,
                               stdout=subprocess.DEVNULL, timeout=120)
            env = dict(os.environ, SQUAD_MUMBLE_ENDPOINT=f"127.0.0.1:{port}",
                       SQUAD_MUMBLE_PASSWORD=password, QT_QPA_PLATFORM="offscreen")
            command = [str(executable)]
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                command += ["-o", str(args.output) + ",junitxml", "-o", "-,txt"]
            result = subprocess.run(command, env=env, timeout=180)
            if result.returncode:
                if container:
                    subprocess.run(["docker", "logs", owner], stdout=log, stderr=subprocess.STDOUT, timeout=10)
                log.flush()
                log.seek(0)
                # The fixture password is ephemeral, but it still has no place in logs.
                print(log.read().replace(password, "[fixture password]"))
            return result.returncode
        finally:
            if server:
                server.terminate()
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
            if container:
                subprocess.run(["docker", "rm", "--force", "--volumes", owner], check=False,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
            log.close()


if __name__ == "__main__":
    raise SystemExit(main())
