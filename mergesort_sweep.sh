#!/bin/bash
# full_sweep.sh
LABEL=$1   # "padded" or "unpadded"
BINARY="./mergesort"

if [ -z "$LABEL" ]; then
    echo "Usage: ./full_sweep.sh <label>"
    exit 1
fi

OUTDIR="mergesort_results"
LOGFILE="$OUTDIR/${LABEL}_full.csv"

THREADS=(1 2 4 6 8 12)
RUNS=60
WARMUP=5

mkdir -p "$OUTDIR"
echo "threads,run,output" > "$LOGFILE"

for t in "${THREADS[@]}"; do
    for r in $(seq 1 $RUNS); do
        output=$($BINARY "$t")
        echo "[$LABEL] threads=$t run=$r -> $output" | tr '\n' ' '
        echo ""

        if [ "$r" -gt "$WARMUP" ]; then
            echo "$t,$r,$output" >> "$LOGFILE"
        fi
    done
done

echo "Done. Results in $LOGFILE"