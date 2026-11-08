#!/usr/bin/env bash
# Multivoid friend installer for Linux and macOS (Voices of the Void 0.10.0 run through Proton / Wine / CrossOver).
#
# Linux twin of Install-Multivoid.ps1: same payload layout, same manifest, same menu.
#   payload/mod/            -> <game>/VotV/Binaries/Win64/Mods/Multivoid/
#   payload/loader/dwmapi.dll -> <game>/VotV/Binaries/Win64/dwmapi.dll   (the Multivoid loader)
#   payload/pak/            -> <game>/VotV/Content/Paks/LogicMods/multivoid/   (optional skins)
#
# Needs: bash (3.2 on macOS is fine), find, grep, sed, awk, od, sha256sum (or shasum / openssl). Nothing is downloaded.
# macOS: VOTV has no Mac build; this installs into the Windows build inside a Wine wrapper (CrossOver, Whisky,
# Sikarugir/Kegworks, plain Wine). Mac support is community-made and untested by the Multivoid authors.
#
#   chmod +x install-multivoid.sh && ./install-multivoid.sh
#
# Unattended:  ./install-multivoid.sh --action install --game-path "$HOME/Games/VotV" --yes
#
# Options:
#   --action install|uninstall|repair   skip the menu
#   --game-path <dir|exe>               the game folder (holding WindowsNoEditor), WindowsNoEditor,
#                                       VotV/Binaries/Win64, or VotV-Win64-Shipping.exe itself
#   --yes, -y                           never ask a question; every question takes its safe default
#   --skip-version-check                go on although the exe is not the 0.10.0 build
#   --ue4ss ask|keep|replace            what to do about an existing UE4SS / foreign dwmapi.dll
#   --no-dll-override                   do not touch the Wine prefix's user.reg (no dwmapi=native,builtin override)
#   --remove-logs                       uninstall only, unattended: also delete logs and bug reports
#   --remove-user-data                  uninstall only, unattended: also delete settings, saves, identity key
#   --payload-dir <dir>                 payload folder somewhere else (default: payload/ beside this script)
#   -h, --help

# Started with `sh install-multivoid.sh`? Re-run under bash (this check is plain POSIX on purpose).
if [ -z "${BASH_VERSION:-}" ]; then
    if command -v bash >/dev/null 2>&1; then exec bash "$0" "$@"; fi
    echo "This installer needs bash." >&2
    exit 1
fi

# ---------------------------------------------------------------------------------------------------------------
# Constants and options
# ---------------------------------------------------------------------------------------------------------------
INSTALLER_VERSION='1.0'
GAME_TARGET='0.10.0'
EXPECTED_STAMP=$((0x6ae37b05))      # PE TimeDateStamp of VotV-Win64-Shipping.exe, 0.10.0
EXE_NAME='VotV-Win64-Shipping.exe'
MANIFEST_NAME='install-manifest.json'
BACKUP_SUFFIX='.multivoid-backup'
OLD_LAYOUT_DIR='multivoid-old-install-backup'
TAB=$(printf '\t')
OVERRIDE_VALUE='native,builtin'

if [ "$(uname -s 2>/dev/null)" = Darwin ]; then
    IS_MAC=1; OS_LABEL=macos; OS_PRETTY='macOS (Wine wrapper)'; GUIDE_NAME='INSTALL-MAC.md'
else
    IS_MAC=0; OS_LABEL=linux; OS_PRETTY='Linux (Proton/Wine)'; GUIDE_NAME='INSTALL-LINUX.md'
fi

ACTION=''
GAME_PATH=''
PAYLOAD_DIR=''
UE4SS_MODE='ask'
SKIP_VERSION_CHECK=0
NO_DLL_OVERRIDE=0
REMOVE_LOGS=0
REMOVE_USER_DATA=0
ASSUME_YES=0
ARGC=$#

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case $1 in
        --action)             shift; ACTION=${1:-} ;;
        --action=*)           ACTION=${1#--action=} ;;
        --game-path)          shift; GAME_PATH=${1:-} ;;
        --game-path=*)        GAME_PATH=${1#--game-path=} ;;
        --payload-dir)        shift; PAYLOAD_DIR=${1:-} ;;
        --payload-dir=*)      PAYLOAD_DIR=${1#--payload-dir=} ;;
        --ue4ss)              shift; UE4SS_MODE=${1:-} ;;
        --ue4ss=*)            UE4SS_MODE=${1#--ue4ss=} ;;
        --yes|-y|--non-interactive) ASSUME_YES=1 ;;
        --skip-version-check) SKIP_VERSION_CHECK=1 ;;
        --no-dll-override)    NO_DLL_OVERRIDE=1 ;;
        --remove-logs)        REMOVE_LOGS=1 ;;
        --remove-user-data)   REMOVE_USER_DATA=1 ;;
        -h|--help)            usage; exit 0 ;;
        *) echo "Unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

ACTION=$(printf '%s' "$ACTION" | tr 'A-Z' 'a-z')
UE4SS_MODE=$(printf '%s' "$UE4SS_MODE" | tr 'A-Z' 'a-z')
case $ACTION in ''|install|uninstall|repair) ;; *) echo "--action must be install, uninstall or repair." >&2; exit 2 ;; esac
case $UE4SS_MODE in ask|keep|replace) ;; *) echo "--ue4ss must be ask, keep or replace." >&2; exit 2 ;; esac

# Unattended when --yes, or when both --action and --game-path are given (same rule as the PowerShell installer).
INTERACTIVE=1
if [ "$ASSUME_YES" = 1 ] || { [ -n "$ACTION" ] && [ -n "$GAME_PATH" ]; }; then INTERACTIVE=0; fi

# ---------------------------------------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------------------------------------
if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_CYAN=$'\033[36m'
    C_GRAY=$'\033[90m'; C_RESET=$'\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_GRAY=''; C_RESET=''
fi

tag()  { printf '%s%-7s%s%s\n' "$1" "$2" "$C_RESET" "$3"; }
step() { printf '\n%s%s%s\n' "$C_CYAN" "$1" "$C_RESET"; }
info() { tag "$C_GRAY" '[..]' "$1"; }
ok()   { tag "$C_GREEN" '[ OK ]' "$1"; }
warn() { tag "$C_YELLOW" '[WARN]' "$1"; }
fail() { tag "$C_RED" '[FAIL]' "$1"; }

die() {
    printf '\n'
    fail "$1"
    exit 1
}

# prompt <text>: reads one line into REPLY (the prompt goes to stderr so it shows even when stdout is captured).
prompt() {
    printf '%s' "$1" >&2
    if ! IFS= read -r REPLY; then
        printf '\n' >&2
        die "Input closed. Run this installer from a terminal (see $GUIDE_NAME), or pass --action and --game-path."
    fi
}

lc() { printf '%s' "$1" | tr 'A-Z' 'a-z'; }

# ask_yn <question> <default 1|0>: returns 0 for yes. Unattended it returns the default.
ask_yn() {
    local q=$1 def=$2 hint a
    if [ "$INTERACTIVE" != 1 ]; then return $((1 - def)); fi
    if [ "$def" = 1 ]; then hint='Y/n'; else hint='y/N'; fi
    while :; do
        prompt "$q [$hint] "
        a=$(lc "${REPLY//[[:space:]]/}")
        if [ -z "$a" ]; then return $((1 - def)); fi
        case $a in
            y|yes) return 0 ;;
            n|no)  return 1 ;;
        esac
        echo 'Please answer y or n.' >&2
    done
}

# ---------------------------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------------------------
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        openssl dgst -sha256 "$1" | sed 's/^.*= *//'
    fi
}

file_size() { wc -c < "$1" | tr -d '[:space:]'; }

# contains_text <file> <text>: fixed-string search; NUL bytes are dropped first, so a UTF-16LE string matches too.
contains_text() { tr -d '\0' < "$1" | LC_ALL=C grep -aqF -- "$2"; }

is_mv_loader() { contains_text "$1" '\Mods\Multivoid\dlls\main.dll'; }

is_mv_xinput() {
    contains_text "$1" 'MULTIVOID_DUP_FILES' || contains_text "$1" 'multivoid-*.dll'
}

# u32le <file> <offset>: little-endian uint32 into U32 (empty and return 1 when the bytes are not there).
u32le() {
    local b
    b=($(od -An -v -t u1 -j "$2" -N 4 "$1" 2>/dev/null))
    if [ "${#b[@]}" -ne 4 ]; then U32=''; return 1; fi
    U32=$((b[0] + b[1] * 256 + b[2] * 65536 + b[3] * 16777216))
}

# get_pe_stamp <exe>: PE TimeDateStamp into PE_STAMP (empty when the file is not a PE).
get_pe_stamp() {
    local f=$1 mz pe
    PE_STAMP=''
    mz=$(od -An -v -t u1 -j 0 -N 2 "$f" 2>/dev/null | tr -s '[:space:]' ' ')
    [ "${mz# }" = '77 90 ' ] || [ "${mz# }" = '77 90' ] || return 1          # "MZ"
    u32le "$f" 60 || return 1                                                  # e_lfanew at 0x3C
    pe=$U32
    u32le "$f" "$pe" || return 1
    [ "$U32" = 17744 ] || return 1                                             # "PE\0\0"
    u32le "$f" $((pe + 8)) || return 1                                         # TimeDateStamp
    PE_STAMP=$U32
}

hex8() { printf '0x%08x' "$1"; }

json_esc() {
    JESC=${1//\\/\\\\}
    JESC=${JESC//\"/\\\"}
}

# ---------------------------------------------------------------------------------------------------------------
# Finding the game
# ---------------------------------------------------------------------------------------------------------------
# resolve_win64 <path>: any of the exe, Win64, VotV, WindowsNoEditor or the folder above -> R_WIN64 (the Win64 dir).
resolve_win64() {
    local p=$1 rel c base
    R_WIN64=''
    p=$(printf '%s' "$p" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    p=${p//\\ / }                                   # a path dragged into a terminal: "My\ Games"
    case $p in \"*\") p=${p#\"}; p=${p%\"} ;; \'*\') p=${p#\'}; p=${p%\'} ;; esac
    case $p in '~'|'~/'*) p="$HOME${p#\~}" ;; esac
    [ -n "$p" ] || return 1
    while [ "${#p}" -gt 1 ] && [ "${p%/}" != "$p" ]; do p=${p%/}; done
    if [ -f "$p" ]; then
        base=$(basename "$p")
        if [ "$(lc "$base")" = "$(lc "$EXE_NAME")" ] && [ "$base" = "$EXE_NAME" ]; then
            R_WIN64=$(cd "$(dirname "$p")" && pwd -P)
            return 0
        fi
        return 1
    fi
    [ -d "$p" ] || return 1
    for rel in '' Binaries/Win64 VotV/Binaries/Win64 WindowsNoEditor/VotV/Binaries/Win64; do
        if [ -n "$rel" ]; then c="$p/$rel"; else c="$p"; fi
        if [ -f "$c/$EXE_NAME" ]; then
            R_WIN64=$(cd "$c" && pwd -P)
            return 0
        fi
    done
    return 1
}

CANDS=()
add_cand() {
    local d=$1 x
    for x in "${CANDS[@]}"; do [ "$x" = "$d" ] && return 0; done
    CANDS+=("$d")
}

# scan_root <dir>: every VotV-Win64-Shipping.exe below it (bounded depth) becomes a candidate.
scan_root() {
    local root=$1 depth=${2:-9} f
    [ -d "$root" ] || return 0
    while IFS= read -r f; do
        if resolve_win64 "$(dirname "$f")"; then add_cand "$R_WIN64"; fi
    done < <(find -L "$root" -maxdepth "$depth" -type f -iname "$EXE_NAME" 2>/dev/null)
}

# scan_prefix <drive_c>: a Wine prefix's C: drive. Deeper than scan_root (Program Files/Steam/steamapps/common/...),
# and the big windows/ system tree is skipped.
scan_prefix() {
    local root=$1 f
    [ -d "$root" ] || return 0
    while IFS= read -r f; do
        if resolve_win64 "$(dirname "$f")"; then add_cand "$R_WIN64"; fi
    done < <(find -L "$root" -maxdepth 12 \( -type d -iname windows -prune \) -o \( -type f -iname "$EXE_NAME" -print \) 2>/dev/null)
}

# scan_steam_common <steamapps/common>: only folders named like the game, as the Windows installer does.
scan_steam_common() {
    local common=$1 d name
    [ -d "$common" ] || return 0
    for d in "$common"/*/; do
        [ -d "$d" ] || continue
        name=$(lc "$(basename "$d")")
        case $name in *void*|*votv*) scan_root "${d%/}" ;; esac
    done
}

# find_candidates_mac: VOTV has no Mac build, so look inside the usual Wine wrappers and the user folders.
find_candidates_mac() {
    local root app base
    CANDS=()
    info 'Looking in CrossOver bottles, Whisky bottles and Sikarugir / Kegworks / Wineskin wrappers...'
    for root in \
        "$HOME/Library/Application Support/CrossOver/Bottles"/*/drive_c \
        "$HOME/Library/Containers/com.isaacmarovitz.Whisky/Bottles"/*/drive_c \
        "$HOME/Library/Application Support/com.isaacmarovitz.Whisky/Bottles"/*/drive_c \
        "$HOME/.wine/drive_c"; do
        scan_prefix "$root"
    done
    for base in "$HOME/Applications" /Applications; do
        for app in Sikarugir Kegworks Wineskin; do
            for root in "$base/$app"/*.app/Contents/SharedSupport/prefix/drive_c; do
                scan_prefix "$root"
            done
        done
    done

    info 'Looking in Games, Downloads, Desktop and Documents (this can take a moment; macOS may ask for folder access)...'
    for root in "$HOME/Games" "$HOME/Downloads" "$HOME/Desktop" "$HOME/Documents"; do
        scan_root "$root"
    done
}

find_candidates() {
    local lib vdf root m h
    CANDS=()
    if [ "$IS_MAC" = 1 ]; then find_candidates_mac; return 0; fi
    local steam_roots=(
        "$HOME/.local/share/Steam"
        "$HOME/.steam/steam"
        "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        "$HOME/.var/app/com.valvesoftware.Steam/.steam/steam"
    )

    info 'Looking in your Steam libraries (non-Steam games live elsewhere; those come next)...'
    for root in "${steam_roots[@]}"; do
        [ -d "$root" ] || continue
        scan_steam_common "$root/steamapps/common"
        vdf="$root/steamapps/libraryfolders.vdf"
        [ -f "$vdf" ] || continue
        while IFS= read -r lib; do
            [ -n "$lib" ] && scan_steam_common "$lib/steamapps/common"
        done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | sed 's/\\\\/\\/g')
    done

    info 'Looking in the usual game folders (Games, Lutris, Heroic, other drives)...'
    for root in \
        "$HOME/Games" \
        "$HOME/.local/share/lutris" \
        "$HOME/.var/app/net.lutris.Lutris/data/lutris" \
        "$HOME/.var/app/com.heroicgameslauncher.hgl/Games" \
        "$HOME/Heroic"; do
        scan_root "$root"
    done
    for m in /run/media/"${USER:-}"/* /media/"${USER:-}"/* /mnt/*; do
        [ -d "$m" ] || continue
        for h in Games SteamLibrary/steamapps/common steamapps/common; do
            if [ "${h##*/}" = common ]; then scan_steam_common "$m/$h"; else scan_root "$m/$h"; fi
        done
    done

    info 'Looking in Downloads and on the Desktop (this can take a moment)...'
    for root in "$HOME/Downloads" "$HOME/Desktop"; do
        scan_root "$root"
    done
}

# Candidates that are the 0.10.0 build first; CAND_OK holds 1/0 per entry.
sort_candidates() {
    local d first=() rest=()
    CAND_OK=()
    for d in "${CANDS[@]}"; do
        get_pe_stamp "$d/$EXE_NAME"
        if [ -n "$PE_STAMP" ] && [ "$PE_STAMP" = "$EXPECTED_STAMP" ]; then first+=("$d"); else rest+=("$d"); fi
    done
    CANDS=()
    for d in "${first[@]}"; do CANDS+=("$d"); CAND_OK+=(1); done
    for d in "${rest[@]}";  do CANDS+=("$d"); CAND_OK+=(0); done
}

select_game() {
    local n i a
    if [ -n "$GAME_PATH" ]; then
        resolve_win64 "$GAME_PATH" || die "No $EXE_NAME found under '$GAME_PATH'. Point --game-path at the game folder."
        WIN64=$R_WIN64
        return 0
    fi
    [ "$INTERACTIVE" = 1 ] || die 'Unattended runs need --game-path.'

    find_candidates
    sort_candidates
    n=${#CANDS[@]}
    if [ "$n" -eq 1 ]; then
        ok "Found the game: ${CANDS[0]}"
        if ask_yn 'Use this one?' 1; then WIN64=${CANDS[0]}; return 0; fi
    elif [ "$n" -gt 1 ]; then
        ok "Found $n copies of the game:"
        for ((i = 0; i < n; i++)); do
            if [ "${CAND_OK[$i]}" = 1 ]; then
                printf '  %d) %s   %s[VOTV %s]%s\n' $((i + 1)) "${CANDS[$i]}" "$C_GREEN" "$GAME_TARGET" "$C_RESET"
            else
                printf '  %d) %s   %s[another version]%s\n' $((i + 1)) "${CANDS[$i]}" "$C_YELLOW" "$C_RESET"
            fi
        done
        while :; do
            prompt 'Pick a number (or press Enter to type a path): '
            a=${REPLY//[[:space:]]/}
            [ -z "$a" ] && break
            case $a in
                *[!0-9]*) ;;
                *) if [ "$a" -ge 1 ] && [ "$a" -le "$n" ]; then WIN64=${CANDS[$((a - 1))]}; return 0; fi ;;
            esac
        done
    else
        warn 'Could not find the game by itself.'
    fi
    while :; do
        prompt "Type or paste the game folder (or the path of $EXE_NAME), empty to quit: "
        [ -n "${REPLY//[[:space:]]/}" ] || die 'No game folder chosen.'
        if resolve_win64 "$REPLY"; then WIN64=$R_WIN64; return 0; fi
        warn "No $EXE_NAME under '$REPLY'. Try again."
    done
}

set_layout() {
    local binaries=${WIN64%/*} votv
    votv=${binaries%/*}
    EXE="$WIN64/$EXE_NAME"
    MOD_DIR="$WIN64/Mods/Multivoid"
    PAK_DIR="$votv/Content/Paks/LogicMods/multivoid"
    MANIFEST="$MOD_DIR/$MANIFEST_NAME"
}

# dest_of <base exe|pak> <rel>: DEST
dest_of() {
    if [ "$1" = pak ]; then DEST="$PAK_DIR/$2"; else DEST="$WIN64/$2"; fi
}

confirm_version() {
    local shown
    get_pe_stamp "$EXE"
    if [ -n "$PE_STAMP" ] && [ "$PE_STAMP" = "$EXPECTED_STAMP" ]; then
        ok "Game build is VOTV $GAME_TARGET (exe stamp $(hex8 "$PE_STAMP"))."
        return 0
    fi
    if [ -z "$PE_STAMP" ]; then shown='unreadable'; else shown=$(hex8 "$PE_STAMP"); fi
    warn "This is NOT the VOTV $GAME_TARGET build this Multivoid was made for (exe stamp $shown, expected $(hex8 "$EXPECTED_STAMP"))."
    warn 'On another version the mod will refuse to start, or misbehave.'
    if [ "$SKIP_VERSION_CHECK" = 1 ]; then warn '--skip-version-check given: going on.'; return 0; fi
    [ "$INTERACTIVE" = 1 ] || die 'Game version mismatch (pass --skip-version-check to override).'
    ask_yn 'Continue anyway?' 0 || die 'Stopped: wrong game version.'
}

# game_pids: RUNNING_PIDS = pids of a running game (Wine/Proton shows the exe in the command line).
game_pids() {
    local pid cmd
    RUNNING_PIDS=''
    command -v pgrep >/dev/null 2>&1 || return 0
    for pid in $(pgrep -f -i 'VotV-Win64-Shipping\.exe' 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        [ "$pid" = "$PPID" ] && continue
        if [ -r "/proc/$pid/cmdline" ]; then
            cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
        else
            cmd=$(ps -p "$pid" -o command= 2>/dev/null)          # macOS has no /proc
        fi
        case $cmd in *install-multivoid*) continue ;; esac     # this script, when the exe path was passed to it
        RUNNING_PIDS="$RUNNING_PIDS $pid"
    done
}

assert_closed() {
    if ! command -v pgrep >/dev/null 2>&1; then
        warn 'pgrep not found: cannot check whether the game is running. Make sure it is closed.'
        return 0
    fi
    while :; do
        game_pids
        [ -z "$RUNNING_PIDS" ] && return 0
        warn "Voices of the Void is running (pid$RUNNING_PIDS). Close it (and Steam's game session) before changing its files."
        [ "$INTERACTIVE" = 1 ] || die 'Close the game first.'
        prompt 'Close the game, then press Enter to check again (q to quit): '
        case $(lc "${REPLY//[[:space:]]/}") in q|quit) die 'Close the game and run the installer again.' ;; esac
    done
}

# ---------------------------------------------------------------------------------------------------------------
# Payload and manifest
# ---------------------------------------------------------------------------------------------------------------
# Payload entries, parallel arrays.
PAY_BASE=(); PAY_REL=(); PAY_SRC=()
PAYLOAD_LOADER=''

get_payload() {
    local root need f rel
    root=${PAYLOAD_DIR:-$SCRIPT_DIR/payload}
    [ -d "$root" ] || die "The payload folder is missing: $root
Run the installer from the unzipped Multivoid installer folder."
    root=$(cd "$root" && pwd)
    for need in mod/dlls/main.dll mod/enabled.txt loader/dwmapi.dll; do
        [ -f "$root/$need" ] || die "The payload is incomplete: $need is missing from $root"
    done
    PAY_BASE=(); PAY_REL=(); PAY_SRC=()
    while IFS= read -r f; do
        rel=${f#"$root/mod/"}
        PAY_BASE+=(exe); PAY_REL+=("Mods/Multivoid/$rel"); PAY_SRC+=("$f")
    done < <(find "$root/mod" -type f | LC_ALL=C sort)
    if [ -d "$root/pak" ]; then
        while IFS= read -r f; do
            rel=${f#"$root/pak/"}
            PAY_BASE+=(pak); PAY_REL+=("$rel"); PAY_SRC+=("$f")
        done < <(find "$root/pak" -type f | LC_ALL=C sort)
    fi
    PAYLOAD_LOADER="$root/loader/dwmapi.dll"
}

# Old manifest, parallel arrays. The JSON is flattened and picked apart with grep/sed: no jq needed.
M_PRESENT=0; M_LOADER=1
M_FBASE=(); M_FPATH=()
M_BORIG=(); M_BBACKUP=(); M_BSHA=()
M_OLDMOVED=()
M_OV_REG=''; M_OV_STATE=''; M_OV_PRIOR=''; M_OV_SEP=0      # the Wine user.reg override an earlier run added

# A manifest path must stay inside the game folder: no absolute paths, no "..".
safe_rel() {
    case $1 in
        ''|/*|..|../*|*/..|*/../*) return 1 ;;
    esac
    return 0
}

read_manifest() {
    local flat line b p o s ov
    M_PRESENT=0; M_LOADER=1
    M_FBASE=(); M_FPATH=(); M_BORIG=(); M_BBACKUP=(); M_BSHA=(); M_OLDMOVED=()
    M_OV_REG=''; M_OV_STATE=''; M_OV_PRIOR=''; M_OV_SEP=0
    [ -f "$MANIFEST" ] || return 0
    flat=$(tr -d '\r\n' < "$MANIFEST")
    case $flat in
        *'"schema"'*'"files"'*) ;;
        *) warn 'The install manifest is unreadable; treating this as a fresh install.'; return 0 ;;
    esac
    M_PRESENT=1
    if printf '%s' "$flat" | grep -Eq '"loader" *: *false'; then M_LOADER=0; fi

    ov=$(printf '%s' "$flat" | grep -Eo '"dllOverride" *: *\{[^}]*\}')
    if [ -n "$ov" ]; then
        M_OV_REG=$(printf '%s' "$ov" | sed -E 's/.*"userReg" *: *"([^"]*)".*/\1/' | sed 's/\\\\/\\/g')
        M_OV_STATE=$(printf '%s' "$ov" | sed -E 's/.*"state" *: *"([^"]*)".*/\1/')
        M_OV_PRIOR=$(printf '%s' "$ov" | sed -E 's/.*"prior" *: *"([^"]*)".*/\1/')
        M_OV_SEP=$(printf '%s' "$ov" | sed -E 's/.*"sep" *: *([01]).*/\1/')
    fi

    while IFS=$'\t' read -r b p; do
        p=$(printf '%s' "$p" | sed -e 's/\\\\/\//g' -e 's/\\/\//g')
        if ! safe_rel "$p"; then warn "Ignoring unsafe path in the manifest: $p"; continue; fi
        M_FBASE+=("$b"); M_FPATH+=("$p")
    done < <(printf '%s' "$flat" | grep -Eo '"base" *: *"[^"]*" *, *"path" *: *"[^"]*"' |
             sed -E 's/.*"base" *: *"([^"]*)".*"path" *: *"([^"]*)".*/\1'"$TAB"'\2/')

    while IFS=$'\t' read -r o b s; do
        o=$(printf '%s' "$o" | sed -e 's/\\\\/\//g' -e 's/\\/\//g')
        b=$(printf '%s' "$b" | sed -e 's/\\\\/\//g' -e 's/\\/\//g')
        if ! safe_rel "$o" || ! safe_rel "$b"; then warn "Ignoring unsafe backup entry in the manifest: $o"; continue; fi
        M_BORIG+=("$o"); M_BBACKUP+=("$b"); M_BSHA+=("$s")
    done < <(printf '%s' "$flat" | grep -Eo '"original" *: *"[^"]*" *, *"backup" *: *"[^"]*" *, *"sha256" *: *"[^"]*"' |
             sed -E 's/.*"original" *: *"([^"]*)".*"backup" *: *"([^"]*)".*"sha256" *: *"([^"]*)".*/\1'"$TAB"'\2'"$TAB"'\3/')

    while IFS=$'\t' read -r o p; do
        M_OLDMOVED+=("$o"$'\t'"$p")
    done < <(printf '%s' "$flat" | grep -Eo '"Original" *: *"[^"]*" *, *"MovedTo" *: *"[^"]*"' |
             sed -E 's/.*"Original" *: *"([^"]*)".*"MovedTo" *: *"([^"]*)".*/\1'"$TAB"'\2/')
}

# Manifest being built.
MF_BASE=(); MF_PATH=(); MF_SHA=(); MF_SIZE=()
BK_ORIG=(); BK_BACKUP=(); BK_SHA=()
OM_LIST=()          # "Original<TAB>MovedTo"

write_manifest() {
    local loader=$1 tmp i n sep
    mkdir -p "$MOD_DIR" || die "Cannot create $MOD_DIR"
    tmp="$MANIFEST.tmp.$$"
    {
        printf '{\n'
        printf '  "schema": 1,\n'
        printf '  "installer": "Multivoid Installer %s (%s)",\n' "$INSTALLER_VERSION" "$OS_LABEL"
        printf '  "gameTarget": "%s",\n' "$GAME_TARGET"
        printf '  "installedAt": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        json_esc "$WIN64"; printf '  "gamePath": "%s",\n' "$JESC"
        printf '  "loader": %s,\n' "$loader"
        if [ -n "$OV_STATE" ] && [ -n "$OV_REG" ]; then
            json_esc "$OV_REG"
            printf '  "dllOverride": {"userReg": "%s", "state": "%s", "prior": "%s", "sep": %s},\n' "$JESC" "$OV_STATE" "$OV_PRIOR" "$OV_SEP"
        fi

        n=${#MF_PATH[@]}
        if [ "$n" -eq 0 ]; then printf '  "files": [],\n'; else
            printf '  "files": [\n'
            for ((i = 0; i < n; i++)); do
                sep=','; [ "$i" -eq $((n - 1)) ] && sep=''
                json_esc "${MF_PATH[$i]}"
                printf '    {"base": "%s", "path": "%s", "sha256": "%s", "size": %s}%s\n' "${MF_BASE[$i]}" "$JESC" "${MF_SHA[$i]}" "${MF_SIZE[$i]}" "$sep"
            done
            printf '  ],\n'
        fi

        n=${#BK_ORIG[@]}
        if [ "$n" -eq 0 ]; then printf '  "backups": [],\n'; else
            printf '  "backups": [\n'
            for ((i = 0; i < n; i++)); do
                sep=','; [ "$i" -eq $((n - 1)) ] && sep=''
                json_esc "${BK_ORIG[$i]}";   local jo=$JESC
                json_esc "${BK_BACKUP[$i]}"; local jb=$JESC
                printf '    {"original": "%s", "backup": "%s", "sha256": "%s"}%s\n' "$jo" "$jb" "${BK_SHA[$i]}" "$sep"
            done
            printf '  ],\n'
        fi

        n=${#OM_LIST[@]}
        if [ "$n" -eq 0 ]; then printf '  "oldLayoutMoved": []\n'; else
            printf '  "oldLayoutMoved": [\n'
            for ((i = 0; i < n; i++)); do
                sep=','; [ "$i" -eq $((n - 1)) ] && sep=''
                local oo=${OM_LIST[$i]%%$'\t'*} mt=${OM_LIST[$i]#*$'\t'}
                json_esc "$oo"; local jo2=$JESC
                json_esc "$mt"; local jm=$JESC
                printf '    {"Original": "%s", "MovedTo": "%s"}%s\n' "$jo2" "$jm" "$sep"
            done
            printf '  ]\n'
        fi
        printf '}\n'
    } > "$tmp" || die "Cannot write the manifest in $MOD_DIR"
    mv -f -- "$tmp" "$MANIFEST" || die "Cannot write $MANIFEST"
}

# ---------------------------------------------------------------------------------------------------------------
# Install / Repair
# ---------------------------------------------------------------------------------------------------------------
# new_backup_name <full path>: NEWBK
new_backup_name() {
    local i=2
    NEWBK="$1$BACKUP_SUFFIX"
    while [ -e "$NEWBK" ]; do NEWBK="$1$BACKUP_SUFFIX$i"; i=$((i + 1)); done
}

# Old layout: multivoid-*.dll / votv-coop.dll beside the exe, and their xinput1_3.dll loader.
OL_ORIG=(); OL_TO=()
move_old_layout() {
    local f names=() victims=() xin xin_ours=0 aside i v
    OL_ORIG=(); OL_TO=()
    while IFS= read -r f; do
        [ -n "$f" ] && victims+=("$f")
    done < <(find "$WIN64" -maxdepth 1 -type f \( -iname 'multivoid-*.dll' -o -iname 'votv-coop.dll' \) 2>/dev/null | LC_ALL=C sort)
    local dll_count=${#victims[@]}
    xin=$(find "$WIN64" -maxdepth 1 -type f -iname 'xinput1_3.dll' 2>/dev/null | head -n 1)
    if [ -n "$xin" ]; then
        is_mv_xinput "$xin" && xin_ours=1
        if [ "$xin_ours" = 1 ] || [ "$dll_count" -gt 0 ]; then victims+=("$xin")
        else warn "xinput1_3.dll beside the exe does not look like Multivoid's, so it is left alone."; fi
    fi
    [ "${#victims[@]}" -gt 0 ] || return 0
    for v in "${victims[@]}"; do names+=("$(basename "$v")"); done
    warn "Old Multivoid install files found beside the exe: $(IFS=,; echo "${names[*]}" | sed 's/,/, /g')"
    if ! ask_yn "Move them into $OLD_LAYOUT_DIR/ (the new build refuses to start beside them)?" 1; then
        warn 'Left in place. The new Multivoid will refuse to start until they are removed.'
        return 0
    fi
    aside="$WIN64/$OLD_LAYOUT_DIR"
    mkdir -p "$aside" || die "Cannot create $aside"
    for v in "${victims[@]}"; do
        f=$(basename "$v")
        rm -f -- "$aside/$f"
        mv -f -- "$v" "$aside/$f" || die "Cannot move $v"
        OL_ORIG+=("$f"); OL_TO+=("$OLD_LAYOUT_DIR/$f")
        ok "Moved aside: $f"
    done
}

# plan_loader <repair 0|1>: PLAN_INSTALL, and PLAN_BORIG/PLAN_BSHA (what to back up first). Touches nothing.
PLAN_INSTALL=1; PLAN_BORIG=(); PLAN_BSHA=()
plan_loader() {
    local repair=$1 dwm dwm_name='' dwm_exists=0 dwm_ours=0 ue4ss=() f rel mode a was_loader
    PLAN_INSTALL=1; PLAN_BORIG=(); PLAN_BSHA=()

    dwm=$(find "$WIN64" -maxdepth 1 -type f -iname 'dwmapi.dll' 2>/dev/null | head -n 1)
    if [ -n "$dwm" ]; then
        dwm_exists=1; dwm_name=$(basename "$dwm")
        if [ "$(sha256_of "$dwm")" = "$(sha256_of "$PAYLOAD_LOADER")" ] || is_mv_loader "$dwm"; then dwm_ours=1; fi
    fi
    while IFS= read -r f; do
        [ -n "$f" ] && ue4ss+=("${f#"$WIN64/"}")
    done < <(find "$WIN64" -maxdepth 2 -type f -iname 'UE4SS.dll' 2>/dev/null | LC_ALL=C sort)

    if [ "${#ue4ss[@]}" -eq 0 ] && { [ "$dwm_exists" = 0 ] || [ "$dwm_ours" = 1 ]; }; then return 0; fi   # clean

    if [ "${#ue4ss[@]}" -gt 0 ]; then warn "UE4SS is installed here ($(IFS=,; echo "${ue4ss[*]}" | sed 's/,/, /g'))."; fi
    if [ "$dwm_exists" = 1 ] && [ "$dwm_ours" = 0 ]; then
        warn "There is already a dwmapi.dll beside the game exe that is not Multivoid's (UE4SS's loader, or another mod's)."
    fi

    mode=$UE4SS_MODE
    if [ "$mode" = ask ] && [ "$repair" = 1 ] && [ "$M_PRESENT" = 1 ] && [ "${#ue4ss[@]}" -gt 0 ]; then
        was_loader=$M_LOADER
        # Only a recorded Keep is inherited. A UE4SS that came back after a Replace is a new situation: ask again.
        if [ "$was_loader" = 0 ]; then
            mode=keep
            info 'Repair keeps the choice made at install time: Keep UE4SS as the loader.'
        fi
    fi
    if [ "$mode" = ask ]; then
        if [ "$INTERACTIVE" != 1 ]; then
            mode=keep
        else
            echo
            echo '  K) Keep UE4SS as the loader. Multivoid does not install its own dwmapi.dll and starts through UE4SS.'
            echo '     Nothing of yours is touched. (You need a UE4SS that works with the game.)'
            echo '  R) Replace it. UE4SS and that dwmapi.dll are renamed *.multivoid-backup, so UE4SS and its other mods'
            echo '     stay OFF until you uninstall Multivoid (uninstall puts them back).'
            while :; do
                prompt 'Keep UE4SS or Replace it? [K/R] '
                a=$(lc "${REPLY//[[:space:]]/}")
                case $a in k|keep) mode=keep; break ;; r|replace) mode=replace; break ;; esac
            done
        fi
    fi
    if [ "$mode" = keep ]; then
        PLAN_INSTALL=0
        warn 'Not installing the Multivoid loader: the mod will start through the existing loader.'
        return 0
    fi
    if [ "$dwm_exists" = 1 ] && [ "$dwm_ours" = 0 ]; then PLAN_BORIG+=("$dwm_name"); PLAN_BSHA+=("$(sha256_of "$dwm")"); fi
    for rel in "${ue4ss[@]}"; do PLAN_BORIG+=("$rel"); PLAN_BSHA+=("$(sha256_of "$WIN64/$rel")"); done
}

# copy_payload_file <base> <rel> <src>: CP_STATUS new|updated|ok, CP_SHA, CP_DEST. Verifies the copy by hash.
copy_payload_file() {
    local base=$1 rel=$2 src=$3 srcsha
    dest_of "$base" "$rel"; CP_DEST=$DEST
    srcsha=$(sha256_of "$src"); CP_SHA=$srcsha; CP_STATUS=new
    if [ -f "$CP_DEST" ]; then
        if [ "$(sha256_of "$CP_DEST")" = "$srcsha" ]; then CP_STATUS=ok; return 0; fi
        CP_STATUS=updated
    fi
    mkdir -p "$(dirname "$CP_DEST")" || die "Cannot create the folder for $rel"
    cp -f -- "$src" "$CP_DEST" || die "Cannot copy $rel"
    [ "$(sha256_of "$CP_DEST")" = "$srcsha" ] || die "Copy verification failed for $rel (the file on disk does not match the payload)."
}

# ---------------------------------------------------------------------------------------------------------------
# Wine prefix DLL override ("dwmapi"="native,builtin" under [Software\\Wine\\DllOverrides] in the prefix's user.reg)
# ---------------------------------------------------------------------------------------------------------------
OV_REG=''; OV_STATE=''; OV_PRIOR=''; OV_SEP=0
OV_PENDING=0                # uninstall could not undo the override (Wine was running): keep the manifest
DLL_OVERRIDE_OK=0

# find_prefix: PREFIX = the Wine prefix holding the game (a folder with drive_c and user.reg above WIN64).
find_prefix() {
    local p
    PREFIX=''
    case $WIN64 in */drive_c/*) ;; *) return 1 ;; esac
    p=${WIN64%%/drive_c/*}
    [ -d "$p/drive_c" ] && [ -f "$p/user.reg" ] || return 1
    PREFIX=$p
}

# wine_idle <what>: 0 when no wineserver runs (or the check is impossible); 1 when the user chose to skip.
wine_idle() {
    command -v pgrep >/dev/null 2>&1 || {
        warn 'pgrep not found: cannot check whether Wine is running. Make sure CrossOver / your Wine wrapper is quit.'
        return 0
    }
    while pgrep -f wineserver >/dev/null 2>&1; do
        warn "Wine is running (wineserver). Quit CrossOver / Whisky / the wrapper (the whole bottle) first: Wine rewrites user.reg when it exits and would undo $1."
        [ "$INTERACTIVE" = 1 ] || return 1
        prompt 'Quit it, then press Enter to check again (s to skip this step): '
        case $(lc "${REPLY//[[:space:]]/}") in s|skip) return 1 ;; esac
    done
    return 0
}

# reg_apply <user.reg> <out>: writes the edited registry to <out>; OV_STATE/OV_PRIOR/OV_SEP describe what was done:
#   added      a dwmapi line was inserted into the existing DllOverrides section
#   replaced   there was another dwmapi value (OV_PRIOR); it was replaced
#   section    the section did not exist; it was appended (OV_SEP=1 when a blank separator line had to be added)
#   unchanged  the line was already ours       fail  the file looks unsafe to edit
reg_apply() {
    local reg=$1 out=$2 st line rest
    st="$out.status"
    MV_VALUE=$OVERRIDE_VALUE MV_TS=$(date +%s) MV_STATUS=$st awk '
        BEGIN { HDR = "[software\\\\wine\\\\dlloverrides]" }
        function is_hdr(l,   t) {
            t = tolower(l)
            if (index(t, HDR) != 1) return 0
            return (length(t) == length(HDR) || substr(t, length(HDR) + 1, 1) == " ")
        }
        function is_key(l) { return index(tolower(l), "\"dwmapi\"=") == 1 }
        { lines[NR] = $0 }
        END {
            n = NR; hs = 0; found = 0; ins = 0; sep = 0; prior = ""; state = "added"
            for (i = 1; i <= n; i++) if (is_hdr(lines[i])) { hs = i; break }
            if (hs) for (j = hs + 1; j <= n && substr(lines[j], 1, 1) != "["; j++) if (is_key(lines[j])) { found = j; break }
            want = "\"dwmapi\"=\"" ENVIRON["MV_VALUE"] "\""
            if (found) {
                cur = substr(lines[found], 10)
                if (tolower(lines[found]) == tolower(want)) state = "unchanged"
                else if (length(cur) >= 2 && substr(cur, 1, 1) == "\"" && substr(cur, length(cur)) == "\"") {
                    prior = substr(cur, 2, length(cur) - 2)
                    if (index(prior, "\"") || index(prior, "\\") || index(prior, "|")) state = "fail"
                    else state = "replaced"
                } else state = "fail"
            } else if (hs) {
                ins = hs
                while (ins + 1 <= n && substr(lines[ins + 1], 1, 1) == "#") ins++
            } else {
                state = "section"
                if (n > 0 && lines[n] != "") sep = 1
            }
            for (i = 1; i <= n; i++) {
                if (state == "replaced" && i == found) print want; else print lines[i]
                if (state == "added" && i == ins) print want
            }
            if (state == "section") {
                if (sep) print ""
                print "[Software\\\\Wine\\\\DllOverrides] " ENVIRON["MV_TS"]
                print want
                print ""
            }
            print state "|" prior "|" sep > (ENVIRON["MV_STATUS"])
        }' "$reg" > "$out" || { rm -f -- "$out" "$st"; return 1; }
    line=$(cat "$st" 2>/dev/null); rm -f -- "$st"
    [ -n "$line" ] || return 1
    OV_STATE=${line%%|*}; rest=${line#*|}
    OV_PRIOR=${rest%|*}; OV_SEP=${rest##*|}
}

# reg_remove <user.reg> <out> <state> <prior> <sep>: undoes reg_apply. RM_RESULT = removed | restored | absent | changed.
reg_remove() {
    local reg=$1 out=$2 st
    st="$out.status"
    MV_VALUE=$OVERRIDE_VALUE MV_STATE=$3 MV_PRIOR=$4 MV_SEP=$5 MV_STATUS=$st awk '
        BEGIN { HDR = "[software\\\\wine\\\\dlloverrides]" }
        function is_hdr(l,   t) {
            t = tolower(l)
            if (index(t, HDR) != 1) return 0
            return (length(t) == length(HDR) || substr(t, length(HDR) + 1, 1) == " ")
        }
        function is_key(l) { return index(tolower(l), "\"dwmapi\"=") == 1 }
        { lines[NR] = $0 }
        END {
            n = NR; hs = 0; found = 0; result = "absent"
            for (i = 1; i <= n; i++) if (is_hdr(lines[i])) { hs = i; break }
            if (hs) for (j = hs + 1; j <= n && substr(lines[j], 1, 1) != "["; j++) if (is_key(lines[j])) { found = j; break }
            want = "\"dwmapi\"=\"" ENVIRON["MV_VALUE"] "\""
            if (found) {
                if (tolower(lines[found]) != tolower(want)) result = "changed"
                else if (ENVIRON["MV_STATE"] == "replaced") {
                    lines[found] = "\"dwmapi\"=\"" ENVIRON["MV_PRIOR"] "\""
                    result = "restored"
                } else {
                    drop[found] = 1; result = "removed"
                    if (ENVIRON["MV_STATE"] == "section") {
                        e = hs
                        while (e + 1 <= n && substr(lines[e + 1], 1, 1) != "[") e++
                        other = 0
                        for (k = hs + 1; k <= e; k++) if (k != found && lines[k] != "" && substr(lines[k], 1, 1) != "#") other = 1
                        if (!other) {
                            for (k = hs; k <= e; k++) drop[k] = 1
                            if (ENVIRON["MV_SEP"] == "1" && hs > 1 && lines[hs - 1] == "") drop[hs - 1] = 1
                        }
                    }
                }
            }
            for (i = 1; i <= n; i++) if (!drop[i]) print lines[i]
            print result > (ENVIRON["MV_STATUS"])
        }' "$reg" > "$out" || { rm -f -- "$out" "$st"; return 1; }
    RM_RESULT=$(cat "$st" 2>/dev/null); rm -f -- "$st"
    [ -n "$RM_RESULT" ] || return 1
}

# apply_dll_override: returns 0 when the override is in place afterwards, 1 when it is left to the user.
apply_dll_override() {
    local reg tmp bak
    OV_REG=''; OV_STATE=''; OV_PRIOR=''; OV_SEP=0; DLL_OVERRIDE_OK=0
    if [ "$NO_DLL_OVERRIDE" = 1 ]; then info 'Not touching the Wine prefix (--no-dll-override).'; return 1; fi
    if ! find_prefix; then
        info 'The game is not inside a Wine prefix I can see (no drive_c + user.reg above it): the dwmapi override is manual.'
        return 1
    fi
    reg="$PREFIX/user.reg"
    ok "Wine prefix: $PREFIX"
    if ! ask_yn "Add the dwmapi=native,builtin DLL override to this prefix's user.reg?" 1; then return 1; fi
    if ! wine_idle 'the override'; then warn 'Skipped the DLL override: Wine is running.'; return 1; fi
    if ! head -n 1 "$reg" 2>/dev/null | grep -q 'WINE REGISTRY'; then
        warn "$reg does not look like a Wine registry file; leaving it alone."
        return 1
    fi
    tmp="$reg.multivoid-tmp.$$"
    if ! reg_apply "$reg" "$tmp"; then warn "Could not edit $reg; leaving it alone."; return 1; fi
    case $OV_STATE in
        unchanged)
            rm -f -- "$tmp"
            ok 'The dwmapi override is already set in this prefix.'
            if [ "$M_OV_REG" = "$reg" ]; then OV_REG=$M_OV_REG; OV_STATE=$M_OV_STATE; OV_PRIOR=$M_OV_PRIOR; OV_SEP=$M_OV_SEP
            else OV_STATE=''; fi
            DLL_OVERRIDE_OK=1
            return 0 ;;
        added|replaced|section) ;;
        *) rm -f -- "$tmp"; warn "The dwmapi entry in $reg has an unusual form; leaving it alone."; OV_STATE=''; return 1 ;;
    esac
    new_backup_name "$reg"; bak=$NEWBK
    if ! cp -p -- "$reg" "$bak"; then rm -f -- "$tmp"; warn "Could not back up $reg; leaving it alone."; OV_STATE=''; return 1; fi
    if ! cat "$tmp" > "$reg"; then
        cat "$bak" > "$reg"; rm -f -- "$tmp"
        warn "Could not write $reg (restored from the backup)."; OV_STATE=''; return 1
    fi
    rm -f -- "$tmp"
    OV_REG=$reg
    ok "dwmapi=native,builtin set in $reg (backup: $(basename "$bak"))"
    DLL_OVERRIDE_OK=1
    return 0
}

# remove_dll_override: uninstall side; M_OV_* come from the manifest.
remove_dll_override() {
    local reg=$M_OV_REG tmp
    if [ ! -f "$reg" ]; then info "Wine registry $reg is gone; no override to undo."; return 0; fi
    if ! wine_idle 'the removal'; then
        warn "Left the dwmapi override in $reg. Quit Wine and run Uninstall again, or remove the \"dwmapi\" line by hand."
        OV_PENDING=1
        return 0
    fi
    tmp="$reg.multivoid-tmp.$$"
    if ! reg_remove "$reg" "$tmp" "$M_OV_STATE" "$M_OV_PRIOR" "$M_OV_SEP"; then
        warn "Could not edit $reg; the dwmapi override stays."
        return 0
    fi
    case $RM_RESULT in
        removed)  cat "$tmp" > "$reg" && ok "removed the dwmapi override from $reg" ;;
        restored) cat "$tmp" > "$reg" && ok "restored the previous dwmapi override ($M_OV_PRIOR) in $reg" ;;
        changed)  warn "The dwmapi entry in $reg is no longer the one Multivoid set; left alone." ;;
        *)        info "No dwmapi override found in $reg." ;;
    esac
    rm -f -- "$tmp"
}

print_override_done() {
    echo
    echo "  ${C_GREEN}Wine will load the Multivoid loader: the dwmapi override is set in the prefix.${C_RESET} No launch option needed."
    echo '  After a run, Mods/Multivoid/loader.log says whether the loader ran.'
    if [ "$IS_MAC" = 1 ]; then
        echo '  Mac note (untested): if the Multiplayer menu or the F1 overlay does not show, see "In-game overlay" in INSTALL-MAC.md.'
    fi
}

print_mac_help() {
    echo
    echo "${C_CYAN}Last step: make Wine load the Multivoid loader (macOS, untested by the Multivoid authors)${C_RESET}"
    echo '  Wine prefers its own built-in dwmapi.dll over the one beside the game. Tell it to use the native one,'
    echo '  with Wine / the bottle fully quit first:'
    echo
    echo '  CrossOver: select the bottle > Wine Configuration > Libraries > type dwmapi > Add > select it > Edit > "Native then Builtin".'
    echo '  Whisky   : bottle > Config > Open Wine Configuration (winecfg) > Libraries > add dwmapi > Edit > "Native then Builtin".'
    echo '  Sikarugir / Kegworks / Wineskin: open the wrapper app > Tools > Config Utility (winecfg) > Libraries > add dwmapi >'
    echo '             Edit > "Native then Builtin" (Wineskin: Advanced > Tools > Config Utility).'
    echo '  Plain Wine / Game Porting Toolkit: WINEDLLOVERRIDES="dwmapi=n,b" wine VotV-Win64-Shipping.exe'
    echo
    echo '  After a run, Mods/Multivoid/loader.log says whether the loader ran. No Multiplayer entry in the menu'
    echo '  usually means the override is missing.'
    echo '  In-game overlay (chat, F1) may not show under D3DMetal. If it is missing, try the DX11 path: add -dx11 to the'
    echo '  game launch command (CrossOver: "Run Command"), and/or turn DXVK on for the bottle. See INSTALL-MAC.md.'
}

print_proton_help() {
    echo
    echo "${C_CYAN}Last step: make Wine / Proton load the Multivoid loader${C_RESET}"
    echo '  Wine prefers its own built-in dwmapi.dll over the one beside the game. Tell it to use the native one.'
    echo
    echo '  Steam (VOTV added as a non-Steam game; VOTV is not on Steam, so there is no "real" Steam app case):'
    echo '    Library > right-click the VOTV shortcut > Properties > Launch Options, paste:'
    echo
    echo '      WINEDLLOVERRIDES="dwmapi=n,b" %command%'
    echo
    echo "    This installer does not edit Steam's shortcuts.vdf (binary file, Steam must be closed). Paste the line yourself."
    echo '    Flatpak Steam is fine, the line is the same.'
    echo
    echo '  Lutris : game > Configure > System options > Environment variables:  WINEDLLOVERRIDES = dwmapi=n,b'
    echo '  Heroic : game > Settings > Advanced > Environment Variables:        WINEDLLOVERRIDES = dwmapi=n,b'
    echo '  Bottles: bottle Settings > DLL overrides > add dwmapi as "Native, Builtin"'
    echo '  Plain Wine: WINEDLLOVERRIDES="dwmapi=n,b" wine VotV-Win64-Shipping.exe'
    echo
    echo '  After a run, Mods/Multivoid/loader.log says whether the loader ran. No Multiplayer entry in the menu'
    echo '  usually means the override above is missing.'
}

do_install() {
    local repair=$1 verb i b src dst rel keep found key n_new=0 n_upd=0 n_ok=0 e old_files_base old_files_path
    if [ "$repair" = 1 ]; then verb=Repair; else verb=Install; fi
    get_payload
    step 'Locating the game'
    select_game
    set_layout
    ok "Game: $WIN64"
    confirm_version
    assert_closed

    read_manifest
    if [ "$repair" = 1 ] && [ "$M_PRESENT" = 0 ] && [ ! -d "$MOD_DIR" ]; then
        die 'Nothing to repair: Multivoid is not installed here. Choose Install instead.'
    fi
    if [ "$repair" = 1 ] && [ "$M_PRESENT" = 0 ]; then warn 'No install manifest found; repairing as a fresh install (a new manifest is written).'; fi

    step "$verb : checking for conflicts"
    move_old_layout
    plan_loader "$repair"

    # --- from here the disk changes --------------------------------------------------------------------------
    step "$verb : writing files"
    MF_BASE=(); MF_PATH=(); MF_SHA=(); MF_SIZE=()
    BK_ORIG=(); BK_BACKUP=(); BK_SHA=(); OM_LIST=()
    for ((i = 0; i < ${#M_BORIG[@]}; i++)); do      # backups from earlier runs stay recorded while they exist
        if [ -f "$WIN64/${M_BBACKUP[$i]}" ]; then BK_ORIG+=("${M_BORIG[$i]}"); BK_BACKUP+=("${M_BBACKUP[$i]}"); BK_SHA+=("${M_BSHA[$i]}"); fi
    done
    for ((i = 0; i < ${#PLAN_BORIG[@]}; i++)); do
        src="$WIN64/${PLAN_BORIG[$i]}"
        new_backup_name "$src"; dst=$NEWBK
        mv -f -- "$src" "$dst" || die "Cannot back up ${PLAN_BORIG[$i]}"
        rel=${dst#"$WIN64/"}
        BK_ORIG+=("${PLAN_BORIG[$i]}"); BK_BACKUP+=("$rel"); BK_SHA+=("${PLAN_BSHA[$i]}")
        ok "Backed up ${PLAN_BORIG[$i]} -> $rel"
    done

    local e_base=("${PAY_BASE[@]}") e_rel=("${PAY_REL[@]}") e_src=("${PAY_SRC[@]}")
    if [ "$PLAN_INSTALL" = 1 ]; then e_base+=(exe); e_rel+=(dwmapi.dll); e_src+=("$PAYLOAD_LOADER"); fi

    for ((i = 0; i < ${#e_rel[@]}; i++)); do
        copy_payload_file "${e_base[$i]}" "${e_rel[$i]}" "${e_src[$i]}"
        MF_BASE+=("${e_base[$i]}"); MF_PATH+=("${e_rel[$i]}"); MF_SHA+=("$CP_SHA"); MF_SIZE+=("$(file_size "$CP_DEST")")
        case $CP_STATUS in
            new)     n_new=$((n_new + 1)); if [ "${e_base[$i]}" = pak ]; then ok "added     LogicMods/multivoid/${e_rel[$i]}"; else ok "added     ${e_rel[$i]}"; fi ;;
            updated) n_upd=$((n_upd + 1)); if [ "$repair" = 1 ]; then ok "restored  ${e_rel[$i]} (was missing or changed)"; else ok "restored  ${e_rel[$i]} (replaced)"; fi ;;
            *)       n_ok=$((n_ok + 1)); if [ "$repair" = 1 ]; then info "unchanged ${e_rel[$i]}"; fi ;;
        esac
    done

    # Files an earlier install owned that this payload no longer has.
    for ((i = 0; i < ${#M_FPATH[@]}; i++)); do
        found=0
        for ((e = 0; e < ${#MF_PATH[@]}; e++)); do
            if [ "${MF_BASE[$e]}" = "${M_FBASE[$i]}" ] && [ "${MF_PATH[$e]}" = "${M_FPATH[$i]}" ]; then found=1; break; fi
        done
        [ "$found" = 1 ] && continue
        dest_of "${M_FBASE[$i]}" "${M_FPATH[$i]}"
        if [ -f "$DEST" ]; then
            if [ "${M_FPATH[$i]}" = dwmapi.dll ] && ! is_mv_loader "$DEST"; then continue; fi
            rm -f -- "$DEST"
            ok "removed   ${M_FPATH[$i]} (no longer part of the payload)"
        fi
    done

    # oldLayoutMoved: the ones moved now, then the ones an earlier run recorded.
    for ((i = 0; i < ${#OL_ORIG[@]}; i++)); do OM_LIST+=("${OL_ORIG[$i]}"$'\t'"${OL_TO[$i]}"); done
    for e in "${M_OLDMOVED[@]}"; do OM_LIST+=("$e"); done

    # The Wine prefix override (only when Multivoid's own loader is the one that starts the mod).
    DLL_OVERRIDE_OK=0; OV_REG=''; OV_STATE=''; OV_PRIOR=''; OV_SEP=0
    if [ "$PLAN_INSTALL" = 1 ]; then
        step "$verb : Wine DLL override"
        apply_dll_override
    fi
    # Nothing new was added this time (override pre-existing or declined): keep what an earlier run recorded.
    if [ -z "$OV_STATE" ] && [ -n "$M_OV_STATE" ] && [ -n "$M_OV_REG" ] && [ "$DLL_OVERRIDE_OK" = 0 ] && [ -f "$M_OV_REG" ]; then
        OV_REG=$M_OV_REG; OV_STATE=$M_OV_STATE; OV_PRIOR=$M_OV_PRIOR; OV_SEP=$M_OV_SEP
    fi

    write_manifest "$([ "$PLAN_INSTALL" = 1 ] && echo true || echo false)"
    ok "Manifest written: Mods/Multivoid/$MANIFEST_NAME"

    step "$verb done"
    echo "  files: $n_new added, $n_upd replaced, $n_ok already correct"
    if [ "$PLAN_INSTALL" = 1 ]; then
        ok 'The Multivoid loader (dwmapi.dll) is in place.'
        if [ "$DLL_OVERRIDE_OK" = 1 ]; then print_override_done
        elif [ "$IS_MAC" = 1 ]; then print_mac_help
        else print_proton_help; fi
    else
        warn 'The mod is installed, but it relies on UE4SS to start it. Start the game through your UE4SS setup.'
        echo '  (With UE4SS under Proton use the launch option from docs/install.md: WINEDLLOVERRIDES="dwmapi.dll,main.dll,UE4SS=n,b" %command%)'
    fi
    echo '  Loader log, after a run: Mods/Multivoid/loader.log   Mod log: multivoid.log (beside the exe)'
}

# ---------------------------------------------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------------------------------------------
prune_empty() {
    [ -d "$1" ] || return 0
    find "$1" -depth -type d -empty -exec rmdir {} \; 2>/dev/null
    return 0
}

do_uninstall() {
    local i removed=0 bak orig f p n
    step 'Locating the game'
    select_game
    set_layout
    ok "Game: $WIN64"
    assert_closed

    read_manifest
    if [ "$M_PRESENT" = 0 ]; then
        die "There is no Mods/Multivoid/$MANIFEST_NAME here, so the installer does not know what it put in.
Nothing was removed. If you installed by hand, delete the Mods/Multivoid folder and dwmapi.dll yourself;
or run Repair first, which writes a manifest, then Uninstall."
    fi

    step 'Uninstall : removing what the installer added'
    for ((i = 0; i < ${#M_FPATH[@]}; i++)); do
        dest_of "${M_FBASE[$i]}" "${M_FPATH[$i]}"
        [ -f "$DEST" ] || continue
        if [ "${M_FPATH[$i]}" = dwmapi.dll ] && ! is_mv_loader "$DEST"; then
            warn "dwmapi.dll is not Multivoid's any more (something replaced it); leaving it alone."
            continue
        fi
        rm -f -- "$DEST" || die "Cannot remove $DEST"
        removed=$((removed + 1))
        ok "removed   ${M_FPATH[$i]}"
    done
    [ "$removed" -eq 0 ] && info 'None of the installed files were present.'

    for ((i = 0; i < ${#M_BORIG[@]}; i++)); do
        bak="$WIN64/${M_BBACKUP[$i]}"; orig="$WIN64/${M_BORIG[$i]}"
        if [ ! -f "$bak" ]; then warn "backup ${M_BBACKUP[$i]} is gone; cannot restore ${M_BORIG[$i]}."; continue; fi
        if [ -e "$orig" ]; then warn "${M_BORIG[$i]} exists again, so the backup ${M_BBACKUP[$i]} is left where it is."; continue; fi
        mv -f -- "$bak" "$orig" || die "Cannot restore ${M_BORIG[$i]}"
        ok "restored  ${M_BORIG[$i]}"
    done
    for p in "${M_OLDMOVED[@]}"; do
        info "Old-layout file ${p%%$'\t'*} stays in $OLD_LAYOUT_DIR/ (delete that folder when you like)."
    done
    OV_PENDING=0
    if [ -n "$M_OV_STATE" ] && [ -n "$M_OV_REG" ]; then remove_dll_override; fi

    # What the installer did not put there: logs, settings, saves, identity.
    local logs=() data=() diag=() has_locale=0 name
    if [ -d "$MOD_DIR" ]; then
        while IFS= read -r f; do
            [ "$f" = "$MANIFEST" ] && continue
            case $(basename "$f") in *.log) logs+=("$f") ;; *) data+=("$f") ;; esac
        done < <(find "$MOD_DIR" -type f 2>/dev/null | LC_ALL=C sort)
    fi
    for name in multivoid.log multivoid.prev.log multivoid-loaded.txt multivoid-compat-report.txt multivoid_reports; do
        [ -e "$WIN64/$name" ] && diag+=("$WIN64/$name")
    done
    [ -e "$WIN64/multivoid_locale" ] && has_locale=1

    if [ "${#logs[@]}" -gt 0 ] || [ "${#diag[@]}" -gt 0 ]; then
        echo
        echo "Multivoid left ${#logs[@]} log file(s) in its folder and ${#diag[@]} diagnostic item(s) beside the exe (logs, saved bug reports)."
        local do_logs=0
        if [ "$INTERACTIVE" = 1 ] && [ "$REMOVE_LOGS" = 0 ]; then ask_yn 'Delete them?' 1 && do_logs=1; else do_logs=$REMOVE_LOGS; fi
        if [ "$do_logs" = 1 ]; then
            for f in "${logs[@]}"; do rm -f -- "$f"; done
            for f in "${diag[@]}"; do rm -rf -- "$f"; done
            ok 'Logs and reports deleted.'
        else info 'Logs and reports kept.'; fi
    fi
    if [ "${#data[@]}" -gt 0 ] || [ "$has_locale" = 1 ]; then
        echo
        echo "Your own Multivoid data is still there (${#data[@]} file(s) in Mods/Multivoid: settings, saved inventories, identity key$([ "$has_locale" = 1 ] && echo ', translations'))."
        warn 'Deleting the identity key (multivoid_identity.key) orphans the inventory hosts stored for you.'
        local do_data=0
        if [ "$INTERACTIVE" = 1 ] && [ "$REMOVE_USER_DATA" = 0 ]; then ask_yn 'Delete your Multivoid settings and data too?' 0 && do_data=1; else do_data=$REMOVE_USER_DATA; fi
        if [ "$do_data" = 1 ]; then
            for f in "${data[@]}"; do rm -f -- "$f"; done
            [ "$has_locale" = 1 ] && rm -rf -- "$WIN64/multivoid_locale"
            ok 'Settings and data deleted.'
        else info 'Settings and data kept (a later reinstall picks them up).'; fi
    fi

    if [ -f "$MANIFEST" ]; then
        n=$(find "$MOD_DIR" -type f ! -path "$MANIFEST" 2>/dev/null | wc -l | tr -d '[:space:]')
        if [ "$OV_PENDING" = 1 ]; then info 'The install manifest stays because the Wine DLL override is still to be removed.'
        elif [ "$n" -eq 0 ]; then rm -f -- "$MANIFEST"
        else info 'The install manifest stays because kept data remains in the mod folder.'; fi
    fi
    prune_empty "$MOD_DIR"
    prune_empty "$PAK_DIR"
    rmdir "$WIN64/Mods" 2>/dev/null
    step 'Uninstall done'
    ok 'The game boots stock again.'
    return 0
}

# ---------------------------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------------------------
show_banner() {
    printf '\n  %sMultivoid installer%s   for Voices of the Void %s on %s   (installer %s)\n\n' \
        "$C_CYAN" "$C_RESET" "$GAME_TARGET" "$OS_PRETTY" "$INSTALLER_VERSION"
}

read_menu_choice() {
    echo '  1) Install'
    echo '  2) Uninstall'
    echo '  3) Repair'
    echo '  Q) Quit'
    while :; do
        prompt 'Pick one: '
        case $(lc "${REPLY//[[:space:]]/}") in
            1) ACTION=install; return 0 ;;
            2) ACTION=uninstall; return 0 ;;
            3) ACTION=repair; return 0 ;;
            q) ACTION=''; return 0 ;;
        esac
    done
}

for tool in find grep sed awk od tr cut wc sort; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 2; }
done
command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1 || command -v openssl >/dev/null 2>&1 ||
    { echo 'Missing required tool: sha256sum (or shasum / openssl)' >&2; exit 2; }

show_banner
if [ -z "$ACTION" ]; then
    [ "$INTERACTIVE" = 1 ] || die 'Unattended runs need --action.'
    read_menu_choice
    if [ -z "$ACTION" ]; then info 'Nothing done.'; exit 0; fi
fi
case $ACTION in
    install)   do_install 0 ;;
    repair)    do_install 1 ;;
    uninstall) do_uninstall ;;
esac

# Started with no arguments (likely a double-click in a terminal that closes at once): let the output be read.
if [ "$ARGC" -eq 0 ] && [ -t 0 ] && [ -t 1 ] && [ -z "${MULTIVOID_LAUNCHER:-}" ]; then
    printf '\nPress Enter to close. '
    IFS= read -r _
fi
exit 0
