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

# Process each image file
# Matches common image extensions case-insensitively
for img in *.[jJ][pP][gG] *.[jJ][pP][eE][gG] *.[pP][nN][gG]; do
    # Skip if no files match the pattern
    [ -e "$img" ] || continue

    # Define absolute paths for the binary
    INPUT_PATH="$(realpath "$img")"
    OUTPUT_PATH="$(realpath "$img")" # Overwrites original for Octolapse to render masked version

    if ! "$BINARY_PATH" "$INPUT_PATH" "$MASK_FILE" "$OUTPUT_PATH"; then
        echo "Error: failed to mask $INPUT_PATH" >&2
        exit 1
    fi
done

echo "Masking complete for $SNAPSHOT_DIR"
