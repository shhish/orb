"""Render Design 2.0 .dc.html boards to PNG with headless Chrome, for overlap review.

usage: dc2png.py SRC_DIR OUT_DIR [Board ...]
"""

import json
import re
import subprocess
import sys
from pathlib import Path

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def parts(text):
    helmet = re.search(r"<helmet>(.*?)</helmet>", text, re.S)
    body = re.search(r"</helmet>(.*)</x-dc>", text, re.S)
    props = re.search(r"data-props='(.*?)'", text, re.S)
    defaults = {}
    if props:
        for key, spec in json.loads(props.group(1).replace("&#39;", "'")).items():
            if isinstance(spec, dict) and "default" in spec:
                defaults[key] = spec["default"]
    return (helmet.group(1) if helmet else ""), body.group(1), defaults


CONTROL = {
    "brightness": dict(
        endX=312.8,
        endY=430.5,
        arcColor="#FFFFFF",
        sunBg="rgba(255,255,255,0.22)",
        sunRing="inset 0 0 0 1.5px rgba(255,255,255,0.7)",
        sunInk="#FFFFFF",
        volBg="rgba(120,120,128,0.26)",
        volRing="none",
        volInk="rgba(235,235,245,0.7)",
    ),
    "volume": dict(
        endX=280.0,
        endY=440.8,
        arcColor="#FF375F",
        sunBg="rgba(120,120,128,0.26)",
        sunRing="none",
        sunInk="rgba(235,235,245,0.7)",
        volBg="rgba(255,55,95,0.26)",
        volRing="inset 0 0 0 1.5px rgba(255,55,95,0.7)",
        volInk="#FF375F",
    ),
}


def render_body(src, name, overrides=None):
    _, body, defaults = parts((src / f"{name}.dc.html").read_text())
    values = dict(defaults)
    if name == "Control":
        values.update(CONTROL[(overrides or {}).get("mode", "brightness")])
    values.update(overrides or {})

    def importer(m):
        attrs = dict(re.findall(r'(\w[\w-]*)="([^"]*)"', m.group(1)))
        child = attrs.pop("name")
        attrs.pop("hint-size", None)
        return render_body(src, child, attrs)

    body = re.sub(r"<dc-import([^>]*)></dc-import>", importer, body)
    return re.sub(
        r"\{\{\s*([\w.]+)\s*\}\}", lambda m: str(values.get(m.group(1), m.group(0))), body
    )


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    names = sys.argv[3:] or sorted(
        p.name[:-8] for p in src.glob("*.dc.html") if p.name != "Main.dc.html"
    )
    for name in names:
        helmet, _, _ = parts((src / f"{name}.dc.html").read_text())
        page = out / f"{name}.html"
        page.write_text(
            f"<!doctype html><html><head><meta charset='utf-8'>{helmet}"
            f"<style>html,body{{margin:0;background:#202020}}</style></head>"
            f"<body>{render_body(src, name)}</body></html>"
        )
        subprocess.run(
            [
                CHROME,
                "--headless=new",
                "--disable-gpu",
                "--hide-scrollbars",
                "--force-device-scale-factor=1",
                "--window-size=466,466",
                "--virtual-time-budget=4000",
                f"--screenshot={out / (name + '.png')}",
                page.as_uri(),
            ],
            check=True,
            capture_output=True,
        )
        print(name)


main()
