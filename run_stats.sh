#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT_DIR/lz_codec"
DATA_DIR="$ROOT_DIR/data"
OUT_DIR="$ROOT_DIR/out_stats"

mkdir -p "$OUT_DIR"

n=0
while [[ -e "$OUT_DIR/stats_${n}.txt" ]]; do
    n=$((n + 1))
done
STATS_FILE="$OUT_DIR/stats_${n}.txt"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

cd "$ROOT_DIR"

make build

{
    echo "stats file: $(basename "$STATS_FILE")"
    echo "binary: $BIN"
    echo "data dir: $DATA_DIR"
    echo "width: 256"
    echo "generated: $(date '+%Y-%m-%d %H:%M:%S')"
    echo

    shopt -s nullglob
    files=("$DATA_DIR"/*.raw)

    if [[ ${#files[@]} -eq 0 ]]; then
        echo "No .raw files found in $DATA_DIR"
        exit 1
    fi

    modes=("" "-m" "-a" "-a -m")

    for mode in "${modes[@]}"; do
        case "$mode" in
            "")    mode_name="base" ;;
            "-m")  mode_name="model" ;;
            "-a")  mode_name="adaptive" ;;
            "-a -m") mode_name="adaptive_model" ;;
            *)     mode_name="unknown" ;;
        esac

        echo "=== MODE: $mode_name ==="
        echo "flags: ${mode:-<none>}"
        echo

        total_orig=0
        total_comp=0
        total_time_ms=0
        declare -A choice_counts=()

        for raw_file in "${files[@]}"; do
            name="$(basename "$raw_file")"
            comp_file="$TMP_DIR/${name%.raw}_${mode_name}.lz"
            dec_file="$TMP_DIR/${name%.raw}_${mode_name}.raw"

            orig_size=$(wc -c < "$raw_file")

            start_ns=$(date +%s%N)
            if [[ -z "$mode" ]]; then
                choice_output="$("$BIN" -c -i "$raw_file" -o "$comp_file" -w 256)"
            else
                # shellcheck disable=SC2206
                extra_flags=($mode)
                choice_output="$("$BIN" -c -i "$raw_file" -o "$comp_file" -w 256 "${extra_flags[@]}")"
            fi
            end_ns=$(date +%s%N)

            choice_line="$(printf '%s\n' "$choice_output" | grep '^CHOICE ' || true)"
            choice_text="${choice_line#CHOICE }"

            if [[ -n "$choice_text" ]]; then
                choice_key="${choice_text%% size=*}"
                choice_counts["$choice_key"]=$(( ${choice_counts["$choice_key"]:-0} + 1 ))
            else
                choice_text="n/a"
                choice_key="n/a"
            fi

            elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
            comp_size=$(wc -c < "$comp_file")
            delta_bytes=$(( comp_size - orig_size ))

            ratio_pct=$(awk -v c="$comp_size" -v o="$orig_size" 'BEGIN { printf "%.2f", (c / o) * 100 }')
            diff_pct=$(awk -v c="$comp_size" -v o="$orig_size" 'BEGIN { printf "%.2f", ((c - o) / o) * 100 }')

            "$BIN" -d -i "$comp_file" -o "$dec_file"
            if cmp -s "$raw_file" "$dec_file"; then
                verify="OK"
            else
                verify="FAIL"
            fi

            printf "%-24s | orig=%8d B | comp=%8d B | diff=%8d B | comp/orig=%7s%% | diff=%7s%% | time=%6d ms | choice=%-50s | %s\n" \
                "$name" "$orig_size" "$comp_size" "$delta_bytes" "$ratio_pct" "$diff_pct" "$elapsed_ms" "$choice_text" "$verify"

            total_orig=$((total_orig + orig_size))
            total_comp=$((total_comp + comp_size))
            total_time_ms=$((total_time_ms + elapsed_ms))
        done

        total_ratio_pct=$(awk -v c="$total_comp" -v o="$total_orig" 'BEGIN { printf "%.2f", (c / o) * 100 }')
        total_diff_bytes=$(( total_comp - total_orig ))
        total_diff_pct=$(awk -v c="$total_comp" -v o="$total_orig" 'BEGIN { printf "%.2f", ((c - o) / o) * 100 }')

        echo
        printf "TOTAL                    | orig=%8d B | comp=%8d B | diff=%8d B | comp/orig=%7s%% | diff=%7s%% | time=%6d ms\n" \
            "$total_orig" "$total_comp" "$total_diff_bytes" "$total_ratio_pct" "$total_diff_pct" "$total_time_ms"
        echo

        echo "CHOICE SUMMARY:"
        for key in "${!choice_counts[@]}"; do
            printf "  %-60s %d\n" "$key" "${choice_counts[$key]}"
        done | sort
        echo
    done
} > "$STATS_FILE"

echo "Saved stats to: $STATS_FILE"