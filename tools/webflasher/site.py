#!/usr/bin/env python3
"""Writes the web flasher page next to the packed boards.

    python tools/webflasher/site.py <dir> [version]

<dir> holds what pack.py wrote for every board (<env>.json, <env>_factory.bin,
<env>_firmware.bin); index.html is added there. Serve <dir> (GitHub Pages):
ESP Web Tools flashes from Chrome / Edge on a computer, over USB.
"""
import html, json, os, sys

# Shown names, by PlatformIO environment; anything missing shows its env name.
NAMES = {
    "M5Stick-C-Plus2": "M5StickC Plus2",
    "M5Stick-C": "M5StickC",
    "M5Stick-CPlus": "M5StickC Plus",
    "M5-Cardputer-Adv": "M5Stack Cardputer Adv",
    "M5-StampS3": "M5Stack StampS3",
    "han_m5stack": "M5Stack Core (HAN)",
    "NerdminerV2": "NerdMiner V2 / LilyGO T-Display S3",
    "NerdminerV2-T-Display_V1": "LilyGO T-Display 1.14\" (V1)",
    "TTGO-T-Display": "TTGO T-Display",
    "TTGO-T-Display-Classic": "TTGO T-Display (classic screen)",
    "NerdminerV2-S3-AMOLED": "LilyGO T-Display S3 AMOLED",
    "NerdminerV2-S3-AMOLED-TOUCH": "LilyGO T-Display S3 AMOLED Touch",
    "NerdminerV2-S3-DONGLE": "LilyGO T-Dongle S3",
    "NerdminerV2-S3-GEEK": "Waveshare ESP32-S3-GEEK",
    "NerdminerV2-T-QT": "LilyGO T-QT Pro",
    "NerdminerV2-T-HMI": "LilyGO T-HMI",
    "Lilygo-T-Embed": "LilyGO T-Embed",
    "LilygoT3V1": "LilyGO T3 V1.6 (OLED)",
    "ESP32-2432S028R": "Cheap Yellow Display ESP32-2432S028R",
    "ESP32-2432S028R-Classic": "Cheap Yellow Display ESP32-2432S028R (classic screen)",
    "ESP32_2432S028_2USB": "Cheap Yellow Display 2.8\" (2 USB)",
    "ESP32-2432S024": "Cheap Yellow Display 2.4\" ESP32-2432S024",
    "ESP32-4in-ST7796": "ESP32 4\" ST7796",
    "ES3C35P": "ES3C35P 3.5\" (ESP32-S3)",
    "wt32-sc01": "WT32-SC01",
    "wt32-sc01-plus": "WT32-SC01 Plus",
    "esp32cam": "ESP32-CAM",
    "ESP32-devKitv1": "ESP32 DevKit V1 (no screen)",
    "ESP32-WROOM-Headless": "ESP32-WROOM (no screen)",
    "ESP32-D0WD-V3-weact": "WeAct ESP32-D0WD-V3",
    "ESP32-S3-devKitv1": "ESP32-S3 DevKit (no screen)",
    "esp32-s3-devkitc1-n32r8": "ESP32-S3 DevKitC-1 N32R8",
    "ESP32-S3-mini-wemos": "Wemos / Lolin S3 Mini",
    "ESP32-S3-mini-weact": "WeAct ESP32-S3 Mini",
    "ESP32-S2-mini-wemos": "Wemos / Lolin S2 Mini",
    "ESP32-C3-super-mini": "ESP32-C3 Super Mini",
    "ESP32-C3-devKitmv1": "ESP32-C3 DevKitM-1",
    "ESP32-C3-042-OLED": "ESP32-C3 0.42\" OLED",
    "ESP32-S3-042-OLED": "ESP32-S3 0.42\" OLED",
    "ESP32-C3-spotpear": "Spotpear ESP32-C3 keychain",
}

PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NerdMiner Flasher</title>
<script type="module" src="https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"></script>
<style>
:root{--bg:#f6f8fa;--card:#fff;--line:#d0d7de;--txt:#1f2328;--mut:#59636e;--acc:#f7931a}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#0d1117;--card:#161b22;--line:#30363d;--txt:#e6edf3;--mut:#8b949e}}
:root[data-theme="dark"]{--bg:#0d1117;--card:#161b22;--line:#30363d;--txt:#e6edf3;--mut:#8b949e}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:640px;margin:0 auto;padding:32px 16px 48px}
h1{font-size:26px;margin:0 0 4px}h1 b{color:var(--acc)}
.mut{color:var(--mut)}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;margin-top:20px}
label{display:block;font-weight:600;margin-bottom:8px}
select{width:100%%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--txt);font-size:16px}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:16px}
button.go{background:var(--acc);color:#000;border:0;border-radius:8px;padding:11px 20px;font-size:16px;font-weight:700;cursor:pointer}
a{color:var(--acc)}
ol{padding-left:20px}li{margin:6px 0}
.warn{display:none;background:#f85149;color:#fff;border-radius:8px;padding:10px 12px;margin-top:16px}
code{background:var(--bg);border:1px solid var(--line);border-radius:4px;padding:0 4px}
</style>
</head>
<body>
<main>
<h1>&#9935; Nerd<b>Miner</b> Modded flasher</h1>
<div class="mut">Version %(version)s &middot; %(count)d boards</div>

<div class="card">
<label for="board">1. Pick your board</label>
<select id="board">%(options)s</select>
<div class="row">
<esp-web-install-button id="inst" manifest="">
  <button slot="activate" class="go">2. Connect &amp; install</button>
  <span slot="unsupported">Your browser can't flash over USB: use Chrome or Edge on a computer.</span>
  <span slot="not-allowed">Flashing needs a secure page (https).</span>
</esp-web-install-button>
<a id="bin" href="#">or download the .bin</a>
</div>
<div class="warn" id="nows">This browser has no Web Serial: open this page in Chrome or Edge on a computer.</div>
</div>

<div class="card">
<b>How it works</b>
<ol>
<li>Plug the board into the computer with a USB <i>data</i> cable, and switch it on.</li>
<li>Pick it above, click <b>Connect &amp; install</b>, choose its serial port. Erasing is only needed for a first install or a board coming from other firmware.</li>
<li>After the reboot, join the Wi-Fi <code>NerdMinerAP</code> (password <code>MineYourCoins</code>) with a phone and fill in your Wi-Fi, pool and BTC address.</li>
</ol>
<div class="mut">No port in the list? Install the USB driver of the board (CH340/CH9102 or CP210x). On some boards, hold BOOT while plugging in.</div>
</div>

<div class="card mut">
Firmware: <a href="%(repo)s">%(repo)s</a>. Flashing uses <a href="https://esphome.github.io/esp-web-tools/">ESP Web Tools</a>; everything runs in your browser.
</div>
</main>
<script>
var sel=document.getElementById('board'),inst=document.getElementById('inst'),bin=document.getElementById('bin');
function pick(){var e=sel.value;inst.setAttribute('manifest',e+'.json');bin.href=e+'_factory.bin';bin.download=e+'_factory.bin';
  try{localStorage.setItem('board',e)}catch(x){}}
try{var last=localStorage.getItem('board');if(last&&[].some.call(sel.options,function(o){return o.value===last}))sel.value=last}catch(x){}
sel.addEventListener('change',pick);pick();
if(!('serial' in navigator))document.getElementById('nows').style.display='block';
</script>
</body>
</html>
"""


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    version = sys.argv[2] if len(sys.argv) > 2 else "dev"
    repo = os.environ.get("REPO_URL", "https://github.com/HenrysCat/NerdMiner-Modded")
    envs = []
    for f in os.listdir(d):
        if f.endswith(".json") and os.path.exists(os.path.join(d, f[:-5] + "_factory.bin")):
            envs.append(f[:-5])
    if not envs:
        raise SystemExit("no packed board in " + d)
    envs.sort(key=lambda e: NAMES.get(e, e).lower())
    chips = {e: json.load(open(os.path.join(d, e + ".json")))["builds"][0]["chipFamily"] for e in envs}
    options = "".join('<option value="%s">%s (%s)</option>' % (html.escape(e), html.escape(NAMES.get(e, e)), chips[e])
                      for e in envs)
    open(os.path.join(d, "index.html"), "w", encoding="utf-8").write(
        PAGE % {"version": html.escape(version), "count": len(envs), "options": options, "repo": html.escape(repo)})
    print("index.html: %d boards" % len(envs))


if __name__ == "__main__":
    main()
