#!/bin/bash
# server-docker/multivoid-start.sh -- start a Multivoid dedicated server under Wine and stay in
# front of it: the panel's console input becomes the server's commands, its log becomes stdout,
# and SIGTERM becomes `stop`. docs/dedicated-server.md is the design.
#
# Environment (the egg's variables; every one optional):
#   SERVER_PORT       UDP port (Pterodactyl's allocation)          -> MULTIVOID_PORT
#   SERVER_NAME       lobby name                                   -> MULTIVOID_SERVER_NAME
#   PASSWORD          empty = open                                 -> MULTIVOID_PASSWORD
#   MAX_PLAYERS       2..10, the server included                   -> MULTIVOID_MAX_PLAYERS
#   SAVE_SLOT         world slot                                   -> MULTIVOID_SAVE_SLOT
#   LIST_PUBLIC       1/0, list on the master                      -> MULTIVOID_LIST_PUBLIC
#   REACH             direct | brokered                            -> MULTIVOID_REACH
#   AUTOSAVE_MINUTES  0 = off                                      -> MULTIVOID_AUTOSAVE_MINUTES
#   GAME_DIR          the uploaded game folder (default /home/container/VotV)
#   USE_XVFB          1 (default) = a virtual X display for Wine's windows; 0 = none
#   RHI               null (default) = -nullrhi; software = Mesa llvmpipe on the Xvfb display
#   LOG_MODE          tail (default) = follow multivoid.log; stdout = the mod's own stdout mirror
#   STOP_TIMEOUT      seconds to wait after `stop` before killing (default 120)
#   EXTRA_ARGS        appended to the game's command line
set -u

GAME_DIR="${GAME_DIR:-/home/container/VotV}"
USE_XVFB="${USE_XVFB:-1}"
RHI="${RHI:-null}"
LOG_MODE="${LOG_MODE:-tail}"
STOP_TIMEOUT="${STOP_TIMEOUT:-120}"

log() { echo "multivoid-start: $*"; }

EXE=$(find "${GAME_DIR}" -maxdepth 6 -type f -name 'VotV-Win64-Shipping.exe' 2>/dev/null | head -n 1)
if [ -z "${EXE}" ]; then
    log "no VotV-Win64-Shipping.exe under ${GAME_DIR}."
    log "Upload YOUR OWN game folder (with UE4SS and Multivoid installed) to ${GAME_DIR};"
    log "the game is not redistributable and this image does not contain it."
    exit 10
fi
EXE_DIR=$(dirname "${EXE}")
MOD_DLL=$(find "${EXE_DIR}" -maxdepth 4 -type f -path '*/Mods/Multivoid/dlls/main.dll' 2>/dev/null | head -n 1)
if [ -z "${MOD_DLL}" ]; then
    log "Multivoid is not installed under ${EXE_DIR} (no Mods/Multivoid/dlls/main.dll)."
    exit 11
fi
MOD_DIR=$(dirname "$(dirname "${MOD_DLL}")")
CMD_FILE="${MOD_DIR}/server_commands.txt"
LOG_FILE="${EXE_DIR}/multivoid.log"
log "game ${EXE}"
log "mod  ${MOD_DIR}"

# UE4SS's own GUI console draws with a real graphics API: off on a server.
UE4SS_INI=$(find "${EXE_DIR}" -maxdepth 2 -type f -name 'UE4SS-settings.ini' 2>/dev/null | head -n 1)
if [ -n "${UE4SS_INI}" ]; then
    sed -i -E 's/^(GuiConsoleEnabled|GuiConsoleVisible|ConsoleEnabled)[[:space:]]*=.*/\1 = 0/' "${UE4SS_INI}"
fi

# The Wine prefix: created once, without the Mono and Gecko prompts.
export WINEPREFIX="${WINEPREFIX:-/home/container/.wine}"
export WINEARCH="${WINEARCH:-win64}"
export WINEDEBUG="${WINEDEBUG:--all}"
# UE4SS loads through a proxy DLL beside the exe: Wine must prefer the native one.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=;dwmapi=n,b;xinput1_3=n,b;dinput8=n,b}"

XVFB_PID=""
if [ "${USE_XVFB}" = "1" ]; then
    export DISPLAY="${DISPLAY:-:99}"
    Xvfb "${DISPLAY}" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
fi
if [ ! -d "${WINEPREFIX}/drive_c" ]; then
    log "creating the Wine prefix (first start only)"
    wineboot --init >/dev/null 2>&1 || true
    wineserver -w || true
fi

# The settings, as the mod reads them (docs/dedicated-server.md, "Configuration").
export MULTIVOID_SERVER=1
[ -n "${SERVER_PORT:-}" ]      && export MULTIVOID_PORT="${SERVER_PORT}"
[ -n "${SERVER_NAME:-}" ]      && export MULTIVOID_SERVER_NAME="${SERVER_NAME}"
[ -n "${PASSWORD+x}" ]         && export MULTIVOID_PASSWORD="${PASSWORD:-}"
[ -n "${MAX_PLAYERS:-}" ]      && export MULTIVOID_MAX_PLAYERS="${MAX_PLAYERS}"
[ -n "${SAVE_SLOT:-}" ]        && export MULTIVOID_SAVE_SLOT="${SAVE_SLOT}"
[ -n "${LIST_PUBLIC:-}" ]      && export MULTIVOID_LIST_PUBLIC="${LIST_PUBLIC}"
[ -n "${REACH:-}" ]            && export MULTIVOID_REACH="${REACH}"
[ -n "${AUTOSAVE_MINUTES:-}" ] && export MULTIVOID_AUTOSAVE_MINUTES="${AUTOSAVE_MINUTES}"
if [ "${LOG_MODE}" = "tail" ]; then export MULTIVOID_STDOUT=0; else export MULTIVOID_STDOUT=1; fi

ARGS=(-nosound -unattended -nosplash -NoVerifyGC -multivoid-server)
if [ "${RHI}" = "software" ]; then
    export LIBGL_ALWAYS_SOFTWARE=1
    ARGS+=(-RenderOffScreen)
else
    ARGS+=(-nullrhi)
fi
# shellcheck disable=SC2206
[ -n "${EXTRA_ARGS:-}" ] && ARGS+=(${EXTRA_ARGS})

# Last run's log moved aside now, so the tail below cannot replay its ready line.
[ -f "${LOG_FILE}" ] && mv -f "${LOG_FILE}" "${EXE_DIR}/multivoid.prev.log"
rm -f "${CMD_FILE}"

cd "${EXE_DIR}" || exit 1
log "starting: wine ${EXE##*/} ${ARGS[*]}"
wine "${EXE}" "${ARGS[@]}" </dev/null &
GAME_PID=$!

TAIL_PID=""
if [ "${LOG_MODE}" = "tail" ]; then
    tail -n +1 -F "${LOG_FILE}" 2>/dev/null &
    TAIL_PID=$!
fi

send() { printf '%s\n' "$1" >> "${CMD_FILE}"; }

STOP_AT=""
on_term() {
    log "signal received -- sending stop"
    send stop
    STOP_AT=$(date +%s)
}
trap on_term TERM INT

# The console: every line of our stdin goes to the server. A background job's stdin is
# /dev/null unless redirected, so the reader takes fd 3, our real stdin.
exec 3<&0
{
    while IFS= read -r line <&3; do
        send "${line}"
    done
} &
READER_PID=$!

# Wait for the game, through signals, and kill it if a stop runs past STOP_TIMEOUT.
CODE=0
while kill -0 "${GAME_PID}" 2>/dev/null; do
    wait "${GAME_PID}" 2>/dev/null
    CODE=$?
    if [ -n "${STOP_AT}" ] && [ $(( $(date +%s) - STOP_AT )) -gt "${STOP_TIMEOUT}" ]; then
        log "the server did not stop within ${STOP_TIMEOUT}s -- killing it"
        kill -9 "${GAME_PID}" 2>/dev/null
        wineserver -k 2>/dev/null
        CODE=137
        break
    fi
    kill -0 "${GAME_PID}" 2>/dev/null && sleep 1
done

kill "${READER_PID}" 2>/dev/null
[ -n "${TAIL_PID}" ] && { sleep 1; kill "${TAIL_PID}" 2>/dev/null; }
wineserver -w 2>/dev/null
[ -n "${XVFB_PID}" ] && kill "${XVFB_PID}" 2>/dev/null
log "the server exited (code ${CODE})"
exit "${CODE}"
