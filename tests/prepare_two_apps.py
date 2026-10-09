from pathlib import Path
import argparse, json, plistlib, shlex, shutil, struct, subprocess, tempfile, uuid
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description="Prepare two isolated native macOS test apps.")
parser.add_argument("source", nargs="?", type=Path, help="Installed squadspeak.app bundle")
parser.add_argument("--sign-identity", default="SquadSpeak Local Development", help="Existing stable code-signing identity. Default: SquadSpeak Local Development.")
args = parser.parse_args()
source = args.source or Path((root / 'build/package-check/latest-install-path.txt').read_text().strip()) / 'squadspeak.app'
destination = Path(tempfile.mkdtemp(prefix='squadspeak-two-apps-', dir='/private/tmp'))
def copy_program(source, target):
    result = shutil.copyfile(source, target)
    shutil.copymode(source, target)
    return result

def set_test_uuid(executable, identifier):
    # Distinct app identities need distinct Mach-O UUIDs for macOS local-network
    # permission tracking. This affects test copies only, before signing.
    data = bytearray(executable.read_bytes())
    if data[:4] != b'\xcf\xfa\xed\xfe':
        raise ValueError("The test source must be a thin 64-bit macOS bundle.")
    commands = struct.unpack_from('<I', data, 16)[0]
    offset = 32
    changed = False
    for _ in range(commands):
        command, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("Invalid Mach-O load command.")
        if command == 0x1b:
            if size != 24 or changed:
                raise ValueError("Invalid Mach-O UUID command.")
            original = uuid.UUID(bytes=bytes(data[offset + 8:offset + 24]))
            data[offset + 8:offset + 24] = uuid.uuid5(original, identifier).bytes
            changed = True
        offset += size
    if not changed:
        raise ValueError("The executable has no UUID.")
    executable.write_bytes(data)

launch = ["#!/bin/sh", "set -eu"]
for tag, name, channel, port, avatar, palette in [('A', 'Ari', 'Ari - local test', 48764, 'mossling', 'ocean'), ('B', 'Bea', 'Bea - local test', 48765, 'courier', 'forest')]:
    app = destination / ('SquadSpeak ' + tag + '.app')
    profile = root / 'build/two-app-test' / tag
    shutil.copytree(source, app, symlinks=True, copy_function=copy_program)
    plist = app / 'Contents/Info.plist'
    data = plistlib.loads(plist.read_bytes())
    data.update(CFBundleIdentifier='app.squadspeak.test.' + tag.lower(), CFBundleName='SquadSpeak ' + tag, CFBundleDisplayName='SquadSpeak ' + tag)
    data.setdefault('LSEnvironment', {})['SQUADSPEAK_SETTINGS_FILE'] = str(profile / 'audio.ini')
    plist.write_bytes(plistlib.dumps(data))
    set_test_uuid(app / "Contents/MacOS" / data["CFBundleExecutable"], data["CFBundleIdentifier"])
    for attribute in ['com.apple.FinderInfo', 'com.apple.ResourceFork']:
        subprocess.run(['/usr/bin/xattr', '-dr', attribute, str(app)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--deep', '--sign', args.sign_identity, str(app)], check=True)
    subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    profile.mkdir(parents=True, exist_ok=True)
    # Separate identities on one speaker/microphone create acoustic feedback:
    # keep first launch quiet until the output reference and permissions are ready.
    if not (profile / 'audio.ini.session.json').exists():
        (profile / 'audio.ini.session.json').write_text(json.dumps(dict(version=1, userName=name, channelName=channel, muted=True, deafened=True, avatar=avatar, theme='system', palette=palette, language='en')) + '\n')
    if not (profile / 'audio.ini.channel.json').exists():
        (profile / 'audio.ini.channel.json').write_text(json.dumps(dict(version=1, approved=[], attempts={}, requestsAllowed=True, servicePort=port)) + '\n')
    launch.append(shlex.join(['/usr/bin/open', '-n', str(app), '--args', '--settings-file', str(profile / 'audio.ini')]))
launcher = destination / 'Start both apps.command'
launcher.write_text('\n'.join(launch) + '\n')
launcher.chmod(0o755)
subprocess.run(['/bin/sh', '-n', str(launcher)], check=True)
(root / 'build/two-app-test/apps-path.txt').write_text(str(destination) + '\n')
print(destination)
