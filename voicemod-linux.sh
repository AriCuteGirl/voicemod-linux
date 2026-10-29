#!/usr/bin/env bash
# Voicemod on Linux: the Windows engine runs under Wine, the UI runs in Electron.
#
#   voicemod-linux.sh setup         create the Wine prefix, download Voicemod, install driver + desktop entries
#   voicemod-linux.sh               start everything and open the Linux UI (development checkout)
#   voicemod-linux.sh start-engine  start virtual devices, the watcher and the hidden engine (used by the UI)
#   voicemod-linux.sh url LINK      hand a voicemod:// link (login, coupons...) to the running engine
#   voicemod-linux.sh engine ARGS   run only the engine with its own (Qt) window, for debugging
#   voicemod-linux.sh stop          stop the engine and the watcher, remove the virtual devices
#   voicemod-linux.sh check         list missing host tools
#
# HERE holds our files (read-only inside the AppImage); DATA holds the prefix, the downloaded
# Voicemod build and the extracted frontend. A development checkout that already has
# ./prefix keeps using itself as DATA.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${VOICEMOD_HOME:-}" ]; then
    if [ -d "$HERE/prefix" ]; then VOICEMOD_HOME="$HERE"; else VOICEMOD_HOME="${XDG_DATA_HOME:-$HOME/.local/share}/voicemod-linux"; fi
fi
DATA="$VOICEMOD_HOME"
export WINEPREFIX="${WINEPREFIX:-$DATA/prefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
# Chromium's sandbox does not work under Wine.
export QTWEBENGINE_CHROMIUM_FLAGS="${QTWEBENGINE_CHROMIUM_FLAGS:---no-sandbox --disable-gpu}"
# shim/version.dll (copied next to Voicemod.exe) fixes the driver handshake in memory.
export WINEDLLOVERRIDES="version=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
VM_DATA="$WINEPREFIX/drive_c/users/$USER/AppData/Local/VoicemodV3"
APP="$VM_DATA/app"
VAD_NAME="Voicemod Virtual Audio Device (WDM)"
REQUIRED_TOOLS="wine wineserver winetricks pactl python3 curl unzip cabextract"

log() { printf '[voicemod-linux] %s\n' "$*" >&2; }

check_tools() {
    local missing=""
    for tool in $REQUIRED_TOOLS; do command -v "$tool" >/dev/null || missing="$missing $tool"; done
    if [ -n "$missing" ]; then
        log "missing host tools:$missing"
        return 1
    fi
}

ensure_devices() {
    if ! pactl list short sinks | grep -q $'\tvoicemod_vad\t'; then
        pactl load-module module-null-sink sink_name=voicemod_vad \
            "sink_properties=device.description=\"$VAD_NAME\"" >/dev/null
    fi
    if ! pactl list short sources | grep -q $'\tvoicemod_mic\t'; then
        pactl load-module module-remap-source master=voicemod_vad.monitor source_name=voicemod_mic \
            "source_properties=device.description=\"$VAD_NAME\"" >/dev/null
    fi
}

remove_devices() {
    pactl list short modules | awk '/sink_name=voicemod_vad|source_name=voicemod_mic/ {print $1}' |
        while read -r id; do pactl unload-module "$id"; done
}

# Reports Linux apps recording from voicemod_mic to the shim (see tools/vad-watch.py).
start_watcher() {
    pgrep -f "vad-watch.py $VM_DATA/vad-clients" >/dev/null && return
    mkdir -p "$VM_DATA"
    : > "$VM_DATA/vad-clients"
    LC_ALL=C nohup python3 -I "$HERE/tools/vad-watch.py" "$VM_DATA/vad-clients" voicemod_mic \
        >/dev/null 2>&1 &
}

# Wine shows the engine as plain "Voicemod.exe ..." (its thread name is Chromium's);
# WINEPREFIX tells ours apart from engines in other prefixes.
engine_pids() {
    local pid prefix
    for pid in $(pgrep -f '^Voicemod\.exe( |$)'); do
        prefix="$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | sed -n 's/^WINEPREFIX=//p')"
        if [ -z "${1:-}" ] || [ "$prefix" = "$WINEPREFIX" ]; then echo "$pid"; fi
    done
}

engine_running() {
    [ -n "$(engine_pids ours)" ]
}

run_engine() {
    cp -f "$HERE/shim/version.dll" "$APP/version.dll"
    cd "$APP"
    wine Voicemod.exe "$@"
}

download_app() {
    local info url version zip
    info="$(curl -fsS -H 'app-name: flagship-launcher' -H 'app-os: Windows' \
        https://api.voicemod.net/v1/nextgen/update/0.0.0)"
    version="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["appVersion"])' <<<"$info")"
    url="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["version"]["update"]["windows"]["file"])' <<<"$info")"
    zip="$DATA/cache/Voicemod_${version}-Windows_Update.zip"
    mkdir -p "$(dirname "$zip")" "$APP"
    if [ ! -f "$zip" ]; then
        log "downloading Voicemod $version (about 240 MB)"
        curl -fL --progress-bar -o "$zip.part" "$url"
        mv "$zip.part" "$zip"
    fi
    log "unpacking Voicemod $version"
    unzip -q -o "$zip" -d "$APP"
    echo "$version" > "$DATA/.voicemod-version"
}

install_desktop_entries() {
    local apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    # Inside the AppImage, entries must point at the AppImage file, not the temporary mount.
    local exec_cmd="\"${APPIMAGE:-$HERE/voicemod-linux.sh}\""
    mkdir -p "$apps"
    cp -f "$HERE/icon.png" "$DATA/voicemod.png" 2>/dev/null || cp -f "$HERE/ui/icon.png" "$DATA/voicemod.png"
    cat > "$apps/voicemod-linux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Voicemod
Comment=Voicemod voice changer (Windows engine under Wine)
Exec=$exec_cmd
Icon=$DATA/voicemod.png
Categories=AudioVideo;Audio;
EOF
    cat > "$apps/voicemod-linux-url.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Voicemod (link handler)
Exec=$exec_cmd url %u
MimeType=x-scheme-handler/voicemod;
NoDisplay=true
EOF
    xdg-mime default voicemod-linux-url.desktop x-scheme-handler/voicemod
    update-desktop-database "$apps" 2>/dev/null || true
}

setup() {
    check_tools
    mkdir -p "$DATA"
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        log "creating Wine prefix"
        wineboot -i
        log "installing Visual C++ runtime (winetricks vcrun2022)"
        winetricks -q vcrun2022
    fi
    [ -f "$APP/Voicemod.exe" ] || download_app

    log "installing stub driver"
    cp -f "$HERE/driver/vmstub.sys" "$WINEPREFIX/drive_c/windows/system32/drivers/"
    wine sc query vmstub >/dev/null 2>&1 ||
        wine sc create vmstub type= kernel start= auto \
            binPath= 'C:\windows\system32\drivers\vmstub.sys' DisplayName= "$VAD_NAME" >/dev/null
    wine regedit /S "$(winepath -w "$HERE/driver/vmstub-device.reg")"
    wine regedit /S "$(winepath -w "$HERE/driver/no-systray.reg")"

    log "extracting the frontend from Voicemod.exe"
    rm -rf "$DATA/frontend" "$DATA/qrc"
    python3 -I "$HERE/tools/extract_qrc.py" "$APP/Voicemod.exe" "$DATA/qrc" >/dev/null
    for dir in "$DATA"/qrc/*; do
        [ -f "$dir/index.html" ] && cp -r "$dir" "$DATA/frontend"
    done
    [ -f "$DATA/frontend/index.html" ] || { log "frontend not found in Voicemod.exe"; exit 1; }

    install_desktop_entries
    log "setup done"
}

start_engine() {
    ensure_devices
    start_watcher
    if ! engine_running; then
        log "starting engine"
        mkdir -p "$VM_DATA/logs"
        VOICEMOD_HIDE_QT=1 run_engine --boot >"$VM_DATA/logs/wine.log" 2>&1 &
    fi
}

case "${1:-}" in
    setup) setup ;;
    check) check_tools ;;
    start-engine) start_engine ;;
    url)
        [ -n "${2:-}" ] || { log "usage: $0 url voicemod://..."; exit 2; }
        # A second Voicemod.exe forwards the link to the running one over its local IPC.
        run_engine "$2" ;;
    engine) shift; ensure_devices; start_watcher; run_engine "$@" ;;
    stop)
        wineserver -k || true
        pkill -f "vad-watch.py $VM_DATA/vad-clients" || true
        # The virtual devices are shared; leave them while another Voicemod engine still runs.
        sleep 1
        [ -n "$(engine_pids)" ] || remove_devices ;;
    "")
        start_engine
        cd "$HERE/ui"
        VOICEMOD_HOME="$DATA" exec electron . ;;
    *) log "unknown command: $1"; exit 2 ;;
esac
