"""Build, sign and publish firmware for the devices' automatic online updates.

Devices check GitHub releases of this repo for <board>.json and install
<board>.bin when its build number is newer and its ECDSA P-256 signature
verifies against the public key compiled into the firmware (src/ota_pubkey.h).
The private key never leaves this PC (gitignored).

Two channels:
  stable  https://github.com/<repo>/releases/latest/download/<board>.json
          the newest normal release: what every device gets.
  beta    https://github.com/<repo>/releases/download/beta/<board>.json
          one pre-release whose files are replaced each time; devices with
          devices switched to beta over USB (serial "b") take it, or stable if newer.

  py -3 ota/release.py genkey     one time: key pair + src/ota_pubkey.h
  py -3 ota/release.py beta       build + sign every board, publish to the beta channel
  py -3 ota/release.py promote    publish the current beta files, unchanged, as stable
  py -3 ota/release.py build [--base-url http://192.168.137.1:8000]
                                  build + sign into ota/release/ only (local test server)

Bump FW_BUILD (and FW_VERSION) in src/config.h before every beta: devices
only install a build number higher than their own.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # esp32-mirror/
KEY = os.path.join(ROOT, "ota", "ota_signing_key.pem")
OUT = os.path.join(ROOT, "ota", "release")
PUBKEY_H = os.path.join(ROOT, "src", "ota_pubkey.h")
REPO = "kfirmaymon84/esp32-mirror-weight-tracker"
BOARDS = {"e32r35t": "e32r35t", "e32r40t": "e32r40t"}   # OTA board id -> pio env


def pio():
    exe = shutil.which("pio") or os.path.expanduser(r"~\.platformio\penv\Scripts\pio.exe")
    if not os.path.exists(exe) and not shutil.which("pio"):
        sys.exit("pio not found")
    return exe


def fw_build():
    cfg = open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8").read()
    m = re.search(r"#define\s+FW_BUILD\s+(\d+)", cfg)
    v = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', cfg)
    if not m or not v:
        sys.exit("FW_BUILD / FW_VERSION not found in src/config.h")
    return int(m.group(1)), v.group(1)


def genkey():
    if os.path.exists(KEY):
        sys.exit(f"{KEY} already exists - refusing to overwrite (devices trust it)")
    key = ec.generate_private_key(ec.SECP256R1())
    with open(KEY, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    write_pubkey_h(key.public_key())
    print(f"wrote {KEY}\n  BACK IT UP somewhere safe: without it no update can ever be signed")
    print(f"wrote {PUBKEY_H}")


def write_pubkey_h(pub):
    pem = pub.public_bytes(serialization.Encoding.PEM,
                           serialization.PublicFormat.SubjectPublicKeyInfo).decode()
    lines = "".join(f'    "{l}\\n"\n' for l in pem.strip().splitlines())
    with open(PUBKEY_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n\n// Public half of the OTA signing key (ota/release.py genkey). Firmware\n"
                "// downloaded by the automatic updater is installed only if its signature\n"
                "// verifies against this key. Safe to publish; the private key is not here.\n"
                f"static const char OTA_PUBKEY_PEM[] =\n{lines};\n")


def secrets_strings():
    """Values from the gitignored secrets.h that must never appear in a release."""
    p = os.path.join(ROOT, "src", "secrets.h")
    if not os.path.exists(p):
        return []
    vals = re.findall(r'#define\s+\w+\s+"([^"]{6,})"', open(p, encoding="utf-8").read())
    return [v.encode() for v in vals]


def build(base_url, channel="stable"):
    if not os.path.exists(KEY):
        sys.exit("no signing key: run  py -3 ota/release.py genkey  first")
    key = serialization.load_pem_private_key(open(KEY, "rb").read(), password=None)
    num, ver = fw_build()
    os.makedirs(OUT, exist_ok=True)
    banned = secrets_strings()
    for board, env in BOARDS.items():
        subprocess.run([pio(), "run", "-e", env], cwd=ROOT, check=True)
        src = os.path.join(ROOT, ".pio", "build", env, "firmware.bin")
        data = open(src, "rb").read()
        leaks = [b.decode(errors="replace")[:12] + "..." for b in banned if b in data]
        if leaks:
            sys.exit(f"{env}: firmware contains personal secrets {leaks} - NOT releasing")
        sha = hashlib.sha256(data).hexdigest()
        msg = f"{board}:{num}:{sha}".encode()
        sig = key.sign(msg, ec.ECDSA(hashes.SHA256()))
        tag = "beta" if channel == "beta" else f"v{num}"
        url = (f"{base_url.rstrip('/')}/{board}.bin" if base_url
               else f"https://github.com/{REPO}/releases/download/{tag}/{board}.bin")
        shutil.copyfile(src, os.path.join(OUT, f"{board}.bin"))
        man = {"board": board, "build": num, "version": ver, "size": len(data), "sha256": sha,
               "sig": base64.b64encode(sig).decode(), "url": url}
        with open(os.path.join(OUT, f"{board}.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(man, f, indent=1)
        print(f"{board}: build {num} ({ver}), {len(data)} bytes, sha256 {sha[:16]}..., signed")
    print(f"release files in {OUT}")


def gh(*args, check=True):
    return subprocess.run(["gh", *args, "--repo", REPO], check=check)


def files():
    return [os.path.join(OUT, f"{b}.{e}") for b in BOARDS for e in ("bin", "json")]


def beta():
    """Build + sign every board and replace the files of the "beta" pre-release."""
    num, ver = fw_build()
    build("", channel="beta")
    title = f"Beta: firmware {ver} (build {num})"
    notes = (f"Beta firmware {ver} (build {num}). Installed by devices with "
             f"the beta channel (set over USB). Promoted to stable when it has proven itself.")
    if gh("release", "view", "beta", check=False).returncode != 0:
        gh("release", "create", "beta", *files(), "--prerelease", "--title", title,
           "--notes", notes)
    else:
        gh("release", "upload", "beta", *files(), "--clobber")
        gh("release", "edit", "beta", "--prerelease", "--title", title, "--notes", notes)
    print(f"beta channel: build {num} ({ver})")


def promote():
    """Publish the beta files, byte for byte, as the stable release v<build>."""
    key = serialization.load_pem_private_key(open(KEY, "rb").read(), password=None)
    tmp = os.path.join(OUT, "promote")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    gh("release", "download", "beta", "--dir", tmp, "--pattern", "*.bin", "--pattern", "*.json")
    builds = set()
    out = []
    for board in BOARDS:
        man = json.load(open(os.path.join(tmp, f"{board}.json"), encoding="utf-8"))
        data = open(os.path.join(tmp, f"{board}.bin"), "rb").read()
        sha = hashlib.sha256(data).hexdigest()
        if man["board"] != board or man["sha256"] != sha or man["size"] != len(data):
            sys.exit(f"{board}: beta files don't match their manifest - not promoting")
        key.public_key().verify(base64.b64decode(man["sig"]),
                                f"{board}:{man['build']}:{sha}".encode(),
                                ec.ECDSA(hashes.SHA256()))       # raises if invalid
        builds.add(man["build"])
        man["url"] = f"https://github.com/{REPO}/releases/download/v{man['build']}/{board}.bin"
        with open(os.path.join(tmp, f"{board}.json"), "w", encoding="utf-8", newline="\n") as f:
            json.dump(man, f, indent=1)
        out += [os.path.join(tmp, f"{board}.bin"), os.path.join(tmp, f"{board}.json")]
    if len(builds) != 1:
        sys.exit(f"beta boards have different builds {sorted(builds)} - not promoting")
    num, ver = builds.pop(), man["version"]
    gh("release", "create", f"v{num}", *out, "--latest", "--title", f"Firmware {ver}",
       "--notes", f"Firmware {ver} (build {num}). Installed automatically by the devices.")
    print(f"stable channel: build {num} ({ver})")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "genkey":
        genkey()
    elif cmd == "build":
        base = sys.argv[sys.argv.index("--base-url") + 1] if "--base-url" in sys.argv else ""
        build(base)
    elif cmd == "beta":
        beta()
    elif cmd == "promote":
        promote()
    else:
        sys.exit(__doc__)
