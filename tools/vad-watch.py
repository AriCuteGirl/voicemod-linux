#!/usr/bin/env python3
"""Tell the Wine side which Linux apps are recording from the Voicemod virtual mic.

On Windows, Voicemod notices Discord & co. through audio sessions on its virtual mic and
only then opens the real microphone. Under Wine those apps are invisible, so this
watcher polls PipeWire and writes one "pid<TAB>name" line per recording app to
STATE_FILE, which shim/version.dll turns into a fake audio session.

It also keeps "music" apps (listed in MUSIC_FILE by `voicemod-ctl music on APP`) linked
into the Voicemod sink, so their sound goes out through the virtual mic as well.

usage: vad-watch.py STATE_FILE [SOURCE_NAME]
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time

POLL_SECONDS = 0.5
SINK = "voicemod_vad"
MUSIC_FILE = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "voicemod-linux-music")


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


def music_apps():
    try:
        with open(MUSIC_FILE, encoding="utf-8") as f:
            return [line.strip().lower() for line in f if line.strip()]
    except FileNotFoundError:
        return []


def pw_link(*args):
    return subprocess.run(["pw-link", *args], capture_output=True, text=True).stdout.splitlines()


def current_links():
    """Set of (output port, input port) pairs from `pw-link -l`."""
    links, port = set(), None
    for line in pw_link("-l"):
        if not line.startswith(" "):
            port = line.strip()
        elif "|->" in line and port:
            links.add((port, line.split("|->", 1)[1].strip()))
    return links


def wanted_music_links(apps):
    """Map output ports of matching apps onto the sink's FL/FR inputs."""
    by_node = {}
    for port in pw_link("-o"):
        node, _, name = port.rpartition(":")
        if node and not node.startswith(SINK) and "voicemod" not in node.lower():
            by_node.setdefault(node, []).append(name)
    wanted = set()
    for node, names in by_node.items():
        if not any(app in node.lower() for app in apps):
            continue
        outputs = sorted(n for n in names if n.startswith("output_"))
        for i, name in enumerate(outputs):
            if re.search(r"(FL|_1)$", name) or (len(outputs) == 2 and i == 0):
                targets = ["FL"]
            elif re.search(r"(FR|_2)$", name) or (len(outputs) == 2 and i == 1):
                targets = ["FR"]
            else:
                targets = ["FL", "FR"]  # mono
            wanted.update((f"{node}:{name}", f"{SINK}:playback_{t}") for t in targets)
    return wanted


def sync_music(made):
    """Create missing music links; drop the ones this watcher made that are no longer wanted."""
    apps = music_apps()
    if not apps and not made:
        return made
    wanted = wanted_music_links(apps) if apps else set()
    existing = current_links()
    for out, inp in wanted - existing:
        subprocess.run(["pw-link", out, inp], capture_output=True)
    for out, inp in (made - wanted) & existing:
        subprocess.run(["pw-link", "-d", out, inp], capture_output=True)
    return wanted


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
    music_links = set()
    while True:
        try:
            current = recorders(source_name)
        except (subprocess.CalledProcessError, json.JSONDecodeError, FileNotFoundError):
            current = last or []
        if current != last:
            write_atomic(state_file, "".join(f"{pid}\t{name}\n" for pid, name in current))
            last = current
        try:
            music_links = sync_music(music_links)
        except FileNotFoundError:  # pw-link missing
            pass
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
