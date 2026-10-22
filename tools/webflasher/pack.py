#!/usr/bin/env python3
"""Packs one built environment for the web flasher and the release.

    python tools/webflasher/pack.py <env> <out_dir> [version]

Reads .pio/build/<env>/{bootloader,partitions,firmware}.bin (after
"pio run -e <env>") and writes to <out_dir>:
  <env>_factory.bin   whole flash image from 0x0 (bootloader, partition table,
                      boot_app0, app): what the browser flasher writes
  <env>_firmware.bin  the app alone, for an update over an existing install
  <env>.json          ESP Web Tools manifest for <env>_factory.bin

The chip comes from the bootloader's image header and the app's offset from
the partition table, so custom partition tables need nothing here.
"""
import glob, json, os, struct, sys

CHIPS = {0x0000: "ESP32", 0x0002: "ESP32-S2", 0x0005: "ESP32-C3", 0x0009: "ESP32-S3"}
BOOTLOADER_AT = {"ESP32": 0x1000, "ESP32-S2": 0x1000, "ESP32-C3": 0x0, "ESP32-S3": 0x0}


def chip_of(bootloader):
    if len(bootloader) < 14 or bootloader[0] != 0xE9:
        raise SystemExit("bootloader.bin is not an ESP image")
    chip_id = struct.unpack_from("<H", bootloader, 12)[0]
    if chip_id not in CHIPS:
        raise SystemExit("unknown chip id 0x%04x in bootloader.bin" % chip_id)
    return CHIPS[chip_id]


def partitions(table):
    """(type, subtype, offset, size, label) for each entry of partitions.bin."""
    out = []
    for i in range(0, len(table), 32):
        e = table[i:i + 32]
        if len(e) < 32 or e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = e[2], e[3], *struct.unpack_from("<II", e, 4)
        out.append((ptype, sub, off, size, e[12:28].rstrip(b"\0").decode(errors="replace")))
    return out


def boot_app0():
    """boot_app0.bin from the Arduino framework package (selects app0)."""
    home = os.environ.get("PLATFORMIO_CORE_DIR", os.path.expanduser("~/.platformio"))
    hits = glob.glob(os.path.join(home, "packages", "framework-arduinoespressif32*", "tools", "partitions", "boot_app0.bin"))
    return open(hits[0], "rb").read() if hits else None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    env, out = sys.argv[1], sys.argv[2]
    version = sys.argv[3] if len(sys.argv) > 3 else "dev"
    build = os.path.join(".pio", "build", env)
    boot = open(os.path.join(build, "bootloader.bin"), "rb").read()
    table = open(os.path.join(build, "partitions.bin"), "rb").read()
    app = open(os.path.join(build, "firmware.bin"), "rb").read()
    chip = chip_of(boot)
    parts = partitions(table)
    apps = [p for p in parts if p[0] == 0]  # app partitions: factory / ota_0 first
    if not apps:
        raise SystemExit("no app partition in partitions.bin")
    app_at, app_size = apps[0][2], apps[0][3]
    if len(app) > app_size:
        raise SystemExit("%s: firmware (%d bytes) larger than its partition (%d)" % (env, len(app), app_size))
    pieces = [(BOOTLOADER_AT[chip], boot), (0x8000, table)]
    otadata = [p for p in parts if p[0] == 1 and p[1] == 0]
    b0 = boot_app0()
    if otadata and b0:
        pieces.append((otadata[0][2], b0))
    pieces.append((app_at, app))

    image = bytearray()
    for at, data in sorted(pieces):
        if len(image) > at:
            raise SystemExit("%s: overlapping pieces at 0x%x" % (env, at))
        image += b"\xff" * (at - len(image)) + data
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, env + "_factory.bin"), "wb").write(image)
    open(os.path.join(out, env + "_firmware.bin"), "wb").write(app)
    manifest = {
        "name": "NerdMiner Modded (%s)" % env,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": chip, "parts": [{"path": env + "_factory.bin", "offset": 0}]}],
    }
    json.dump(manifest, open(os.path.join(out, env + ".json"), "w"), indent=2)
    print("%s: %s, app at 0x%x (%d of %d bytes), image %d bytes" % (env, chip, app_at, len(app), app_size, len(image)))


if __name__ == "__main__":
    main()
