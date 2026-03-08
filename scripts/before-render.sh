#!/bin/bash --login

# Octolapse Before Render Script
# Arguments passed by Octolapse:
# $1: Camera Name
# $2: Snapshot Directory (Absolute Path)
# $3: Snapshot Filename Template
# $4: Snapshot Full Path Template

SNAPSHOT_DIR="$2"
MASK_FILE="/home/default/octolapse/mask.png"  # EDIT THIS PATH
BINARY_PATH="/usr/local/bin/masking_client" # EDIT THIS PATH

# Load .env from the script's directory or its parent (if present).
# Values already in the environment take precedence.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for _env_file in "$SCRIPT_DIR/.env" "$SCRIPT_DIR/../.env"; do
    if [ -f "$_env_file" ]; then
        set -a
        # shellcheck source=/dev/null
        source "$_env_file"
        set +a
        break
    fi
done
unset _env_file SCRIPT_DIR

# Progress reporting — set OCTOPRINT_API_KEY in the environment or .env file.
# Leave OCTOPRINT_API_KEY empty to disable progress updates silently.
OCTOPRINT_HOST="${OCTOPRINT_HOST:-http://localhost:5000}"
OCTOPRINT_API_KEY="${OCTOPRINT_API_KEY:-}"

send_progress() {
    local percent="$1"
    [ -z "$OCTOPRINT_API_KEY" ] && return 0
    curl -sf -X POST \
        -H "Content-Type: application/json" \
        -H "X-Api-Key: $OCTOPRINT_API_KEY" \
        -d "{\"percent\": $percent}" \
        "$OCTOPRINT_HOST/api/plugin/octolapse/renderProgress" > /dev/null
}

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

# Wait for jobs as they complete (wait -n: any job), reporting progress after each.
# Build a pid→index map so we can identify which image failed.
declare -A pid_to_idx
for i in "${!pids[@]}"; do
    pid_to_idx[${pids[$i]}]=$i
done

total=${#pids[@]}
completed=0
failed=0

send_progress 0

while [ $completed -lt $total ]; do
    # Wait for any background job to finish; capture its exit status and PID.
    wait -n -p finished_pid
    exit_status=$?
    completed=$((completed + 1))

    if [ $exit_status -ne 0 ]; then
        idx=${pid_to_idx[$finished_pid]}
        echo "Error: failed to mask ${imgs[$idx]}" >&2
        failed=1
    fi

    percent=$(( completed * 100 / total ))
    send_progress "$percent"
done

[ "$failed" -eq 0 ] || exit 1
echo "Masking complete for $SNAPSHOT_DIR ($total images)"
