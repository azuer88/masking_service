#!/bin/bash --login

# Octolapse Before Render Script
# Arguments passed by Octolapse:
# $1: Camera Name
# $2: Snapshot Directory (Absolute Path)
# $3: Snapshot Filename Template
# $4: Snapshot Full Path Template

SNAPSHOT_DIR="$2"
MASK_FILE="/home/default/octolapse/mask.png"  # EDIT THIS PATH
BINARY_PATH="/home/default/masking_service/bin/masking_client" # EDIT THIS PATH

# Validate prerequisites
[ -x "$BINARY_PATH" ] || { echo "Error: client binary not found: $BINARY_PATH" >&2; exit 1; }
[ -f "$MASK_FILE"   ] || { echo "Error: mask file not found: $MASK_FILE" >&2; exit 1; }

# Ensure the snapshot directory exists
if [ ! -d "$SNAPSHOT_DIR" ]; then
    echo "Error: Snapshot directory $SNAPSHOT_DIR does not exist." >&2
    exit 1
fi

# Navigate to the snapshots
cd "$SNAPSHOT_DIR" || exit 1

# Process images in parallel to keep all workers busy.
# Each masking_client call is backgrounded; PIDs are collected so we can
# detect failures after all jobs finish.
pids=()
imgs=()

for img in *.[jJ][pP][gG] *.[jJ][pP][eE][gG] *.[pP][nN][gG]; do
    [ -e "$img" ] || continue
    INPUT_PATH="$(realpath "$img")"
    "$BINARY_PATH" "$INPUT_PATH" "$MASK_FILE" "$INPUT_PATH" &
    pids+=($!)
    imgs+=("$INPUT_PATH")
done

if [ ${#pids[@]} -eq 0 ]; then
    echo "No images found in $SNAPSHOT_DIR"
    exit 0
fi

# Wait for all jobs and report any failures
failed=0
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "Error: failed to mask ${imgs[$i]}" >&2
        failed=1
    fi
done

[ "$failed" -eq 0 ] || exit 1
echo "Masking complete for $SNAPSHOT_DIR (${#pids[@]} images)"
