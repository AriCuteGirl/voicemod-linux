#!/usr/bin/env python3
"""Tell the Wine side which Linux apps are recording from the Voicemod virtual mic.

On Windows, Voicemod notices Discord & co. through audio sessions on its virtual mic and
only then opens the real microphone. Under Wine those apps are invisible, so this
watcher polls PipeWire and writes one "pid<TAB>name" line per recording app to
STATE_FILE, which shim/version.dll turns into a fake audio session.

usage: vad-watch.py STATE_FILE [SOURCE_NAME]
"""
import json
import os
import subprocess
import sys
import time

POLL_SECONDS = 0.5


def pactl_json(*args):
    out = subprocess.run(["pactl", "-f", "json", *args], capture_output=True, text=True, check=True)
    return json.loads(out.stdout or "[]")


def recorders(source_name):
    sources = {s["index"]: s["name"] for s in pactl_json("list", "sources")}
    found = []
    for out in pactl_json("list", "source-outputs"):
        props = out.get("properties", {})
        if sources.get(out.get("source")) != source_name:
            continue
        # Voicemod's own VAD monitor runs under Wine; it must not count as a client.
        if props.get("application.process.binary", "").startswith("wine"):
            continue
        name = props.get("application.name") or props.get("application.process.binary") or "unknown"
        found.append((props.get("application.process.id", "0"), name))
    return sorted(found)


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    state_file = sys.argv[1]
    source_name = sys.argv[2] if len(sys.argv) > 2 else "voicemod_mic"
    last = None
    while True:
        try:
            current = recorders(source_name)
        except (subprocess.CalledProcessError, json.JSONDecodeError, FileNotFoundError):
            current = last or []
        if current != last:
            write_atomic(state_file, "".join(f"{pid}\t{name}\n" for pid, name in current))
            last = current
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
