#!/usr/bin/env bash
set -euo pipefail

home_dir="${HOME:?HOME must be set}"
steam_root="${STEAM_ROOT:-$home_dir/.steam/debian-installation}"
eve_client_dir="${EVE_CLIENT_DIR:-$steam_root/steamapps/compatdata/3800712765/pfx/drive_c/Program Files (x86)/CCP/EVE}"
proton_bin="${PROTON_BIN:-$steam_root/steamapps/common/Proton 7.0/proton}"
compat_data="${EVE_TEST_COMPAT_DATA:-$home_dir/.local/share/evemu-crucible-test/compatdata}"
target_server="${EVE_TEST_SERVER:-192.168.1.158}"
target_port="${EVE_TEST_PORT:-26000}"

start_ini="$eve_client_dir/start.ini"
client_exe="$eve_client_dir/bin/ExeFile.exe"

if [[ ! -x "$proton_bin" ]]; then
    printf 'Proton executable not found: %s\n' "$proton_bin" >&2
    exit 1
fi

if [[ ! -f "$start_ini" || ! -f "$client_exe" ]]; then
    printf 'Crucible client files not found under: %s\n' "$eve_client_dir" >&2
    exit 1
fi

actual_server="$(awk -F= '$1 == "server" { sub(/\r$/, "", $2); print $2; exit }' "$start_ini")"
actual_port="$(awk -F= '$1 == "port" { sub(/\r$/, "", $2); print $2; exit }' "$start_ini")"
if [[ "$actual_server" != "$target_server" || "$actual_port" != "$target_port" ]]; then
    printf 'Refusing to launch: %s points to %s:%s; expected %s:%s\n' \
        "$start_ini" "${actual_server:-<missing>}" "${actual_port:-<missing>}" \
        "$target_server" "$target_port" >&2
    exit 2
fi

if [[ "${1:-}" == "--dry-run" ]]; then
    printf 'Client: %s\nProton: %s\nPrefix: %s\nServer: %s:%s\nRenderer: WineD3D\n' \
        "$client_exe" "$proton_bin" "$compat_data" "$target_server" "$target_port"
    exit 0
fi

mkdir -p "$compat_data"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam_root"
export STEAM_COMPAT_DATA_PATH="$compat_data"
export PROTON_USE_WINED3D=1

cd "$eve_client_dir"
exec "$proton_bin" run ./bin/ExeFile.exe
