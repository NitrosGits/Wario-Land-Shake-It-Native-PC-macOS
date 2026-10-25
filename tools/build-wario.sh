#!/usr/bin/env bash
# WarioRecomp: macOS build (translator -> translation -> compilation -> optional test run).
# Counterpart of tools/build-wario.ps1. Needs Xcode command line tools, CMake, Ninja and a .NET SDK
# (8 or newer: the translator is built for net8.0 and rolled forward). Dependencies (Dawn, SDL3, ...)
# are fetched by CMake itself.
#
# Usage: tools/build-wario.sh [--game-data DIR] [--arch arm64|x86_64] [--jobs N]
#                             [--skip-translate] [--audit] [--run SECONDS]
#   --game-data DIR   extracted game folder (contains sys/main.dol); default: paths.dvd_root in Play/UserData/Config.toml
#   --audit           configure with MKW_BUILD_PRODUCTS=OFF: compile-only check of the native runtime
#                     (needs neither the game nor a translation)
#   --skip-translate  reuse generated/ from an earlier run
#   --run SECONDS     start the result for that long as a smoke test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
logs="$root/build-logs"
project="projects/wario/recomp.yml"
play="$root/Play"
arch="$(uname -m)"
jobs="$(sysctl -n hw.ncpu)"
game_data=""
skip_translate=0
audit=0
run_seconds=0
expected_sha="053be60dde84c8042f45ed2f4ba80c17efb2d20fedf95a414c2c7e37d336912a"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-data) game_data="$2"; shift 2 ;;
        --arch) arch="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        --skip-translate) skip_translate=1; shift ;;
        --audit) audit=1; shift ;;
        --run) run_seconds="$2"; shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

build="$root/nb-mac-$arch"
[[ $audit -eq 1 ]] && build="$root/nb-mac-$arch-audit"
mkdir -p "$logs"
: > "$logs/status.txt"

status() {
    local line; line="$(date '+%Y-%m-%d %H:%M:%S') $*"
    echo "$line"; echo "$line" >> "$logs/status.txt"
}
step() { # step NAME CMD...  (output goes to build-logs/NAME.log)
    local name="$1"; shift
    status "$name: started"
    if ! "$@" > "$logs/$name.log" 2>&1; then
        status "$name: FAILED, see build-logs/$name.log"
        tail -n 40 "$logs/$name.log" >&2
        exit 1
    fi
    status "$name: ok"
}

# Each translated-code shard needs ~1.5 GB of RAM to compile.
ram_gb=$(( $(sysctl -n hw.memsize) / 1073741824 ))
shard_jobs=$(( (ram_gb - 6) / 2 )); (( shard_jobs < 1 )) && shard_jobs=1; (( shard_jobs > jobs )) && shard_jobs=$jobs
status "build started in $root (arch $arch, $jobs jobs, $shard_jobs for translated code, RAM ${ram_gb} GB)"

for tool in cmake ninja clang; do
    command -v "$tool" > /dev/null || { status "ERROR: $tool not found (brew install cmake ninja; xcode-select --install)"; exit 1; }
done

if [[ $audit -eq 0 ]]; then
    command -v dotnet > /dev/null || { status "ERROR: dotnet not found (brew install dotnet)"; exit 1; }

    # 0. Game: main.dol is used in place of the disc; only the DOL is copied.
    if [[ -z "$game_data" && -f "$play/UserData/Config.toml" ]]; then
        game_data="$(sed -n 's/^[[:space:]]*dvd_root[[:space:]]*=[[:space:]]*"\(.*\)".*/\1/p' "$play/UserData/Config.toml" | head -n1)"
    fi
    dol="$root/game/main.dol"
    if [[ ! -f "$dol" ]]; then
        if [[ -z "$game_data" || ! -f "$game_data/sys/main.dol" ]]; then
            status "ERROR: game not found: pass --game-data <folder containing sys/main.dol>"; exit 1
        fi
        mkdir -p "$root/game"; cp "$game_data/sys/main.dol" "$dol"
    fi
    if [[ "$(shasum -a 256 "$dol" | cut -d' ' -f1)" != "$expected_sha" ]]; then
        status "ERROR: main.dol is not RWLE01 (SHA-256 mismatch)"; exit 1
    fi

    # 1. Translator (net8.0 target; newer SDKs run it through roll-forward).
    export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_ROLL_FORWARD=Major
    tr_dll="$root/fw/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
    if [[ ! -f "$tr_dll" ]]; then
        step translator-build dotnet build "$root/fw/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release
    fi
    tr=(dotnet "$tr_dll")

    # 2. Translation
    if [[ $skip_translate -eq 0 ]]; then
        cd "$root"
        step translate "${tr[@]}" translate-recursive 0x80006124 --project "$project" \
            --outdir generated/functions --output-metadata generated/base_translation_output.json \
            --production-source-bundle generated/base_translation_sources.bin \
            --no-function-files --prune-stale --threads "$jobs"
        step data-init "${tr[@]}" generate-data-init --project "$project"
        step shards "${tr[@]}" emit-build-shards --project "$project" \
            --base-metadata "$root/generated/base_translation_output.json" \
            --base-functions-dir "$root/generated/functions" \
            --native-source-dir "$root/runtime/src" --out "$root/generated/build_shards"
    fi
fi

# 3. Configure and build
cfg=(-S "$root/runtime" -B "$build" -G Ninja
     -DCMAKE_BUILD_TYPE=Release
     -DCMAKE_OSX_ARCHITECTURES="$arch"
     -DAURORA_DAWN_PROVIDER=package -DAURORA_SDL3_PROVIDER=vendor
     -DCMAKE_POLICY_DEFAULT_CMP0168=NEW
     -DMKW_TRANSLATED_COMPILE_JOBS="$shard_jobs")
[[ $audit -eq 1 ]] && cfg+=(-DMKW_BUILD_PRODUCTS=OFF)
step configure cmake "${cfg[@]}"

if [[ $audit -eq 1 ]]; then
    step compile cmake --build "$build" --parallel "$jobs"
    status "DONE (audit build in $build)"
    exit 0
fi
step compile cmake --build "$build" --target WiiCompiled --parallel "$jobs"
status "built: $build/WiiCompiled"

# 4. Portable Play folder
mkdir -p "$play/UserData"
cp -f "$build/WiiCompiled" "$play/WarioRecomp"
find "$build" -maxdepth 1 -name '*.dylib' -exec cp -f {} "$play/" \;
echo "WarioRecomp portable" > "$play/portable.txt"
if [[ ! -f "$play/UserData/Config.toml" && -n "$game_data" ]]; then
    printf '[paths]\ndvd_root = "%s"\n' "$game_data" > "$play/UserData/Config.toml"
fi
status "copied to Play/WarioRecomp"

if [[ $run_seconds -gt 0 ]]; then
    echo "$run_seconds" > "$play/autoexit.txt"
    status "test: running ${run_seconds}s"
    (cd "$play" && ./WarioRecomp > "$logs/run.log" 2> "$logs/run.err.log") || status "test: exit code $?"
    rm -f "$play/autoexit.txt"
    status "test finished; see build-logs/run.log and run.err.log"
fi
status "DONE"
