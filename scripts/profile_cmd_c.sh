#!/bin/bash
#
# profile_cmd_c.sh - Profile tracer cmd_c mode with perf and generate flame graph
#
# Usage: ./scripts/profile_cmd_c.sh <config.json>
# Example: ./scripts/profile_cmd_c.sh tests/loop_inf.json

set -e

# Configuration
PERF_DURATION=10
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
FLAMEGRAPH_DIR="$PROJECT_ROOT/tools/FlameGraph"

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <config.json>"
    echo "Example: $0 tests/loop_inf.json"
    exit 1
fi

CONFIG_FILE="$1"

# Check if config file exists
if [ ! -f "$CONFIG_FILE" ]; then
    if [ -f "$PROJECT_ROOT/$CONFIG_FILE" ]; then
        CONFIG_FILE="$PROJECT_ROOT/$CONFIG_FILE"
    else
        echo "Error: Config file not found: $1"
        exit 1
    fi
fi

# Check if tracer exists
TRACER="$PROJECT_ROOT/tracer"
if [ ! -x "$TRACER" ]; then
    echo "Error: tracer not found at $TRACER"
    echo "Please build first with: make"
    exit 1
fi

# Check FlameGraph tools
if [ ! -x "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]; then
    echo "Error: FlameGraph tools not found at $FLAMEGRAPH_DIR"
    exit 1
fi

# Check perf
if ! command -v perf &> /dev/null; then
    echo "Error: perf not found. Please install linux-tools-common"
    exit 1
fi

# Check perf permissions
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 3)
if [ "$PARANOID" -gt 1 ]; then
    echo "Warning: perf_event_paranoid=$PARANOID, attempting to adjust..."
    sudo sysctl kernel.perf_event_paranoid=1 2>/dev/null || {
        echo "Failed to adjust perf_event_paranoid. You may need to run with sudo."
    }
fi

# Generate timestamp and output filename
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
CONFIG_BASENAME=$(basename "$CONFIG_FILE" .json)
OUTPUT_SVG="${PROJECT_ROOT}/flamegraph_${CONFIG_BASENAME}_${TIMESTAMP}.svg"

# Extract config info
CONFIG_INFO=""
if command -v python3 &> /dev/null; then
    CONFIG_INFO=$(python3 -c "import json; d=json.load(open('$CONFIG_FILE')); n=d.get('Nodes', '?'); print(f'Nodes={n}')" 2>/dev/null || echo "")
elif command -v jq &> /dev/null; then
    CONFIG_INFO=$(jq -r '"Nodes=\(.Nodes)"' "$CONFIG_FILE" 2>/dev/null || echo "")
fi
[ -z "$CONFIG_INFO" ] && CONFIG_INFO="Nodes=?"

echo "========================================"
echo "Profiling tracer with cmd_c mode"
echo "Config: $CONFIG_FILE"
echo "Config Info: $CONFIG_INFO"
echo "Duration: ${PERF_DURATION}s"
echo "Output: $OUTPUT_SVG"
echo "========================================"

# Create temp directory
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# Method: Start tracer with stdin redirected from a FIFO
# We write 'c' to the FIFO to trigger cmd_c, then start perf

FIFO="$TMPDIR/tracer_fifo"
PIDFILE="$TMPDIR/tracer.pid"

mkfifo "$FIFO"

cd "$PROJECT_ROOT"

# Start tracer with stdin from FIFO
# Use 'exec' to keep it in the same process group but we can still control it
"$TRACER" -c "$CONFIG_FILE" < "$FIFO" &
TRACER_PID=$!

# Verify tracer started
if ! kill -0 "$TRACER_PID" 2>/dev/null; then
    echo "Error: Failed to start tracer"
    exit 1
fi

echo "Tracer started with PID: $TRACER_PID"
echo "Waiting for tracer to initialize..."

# Give tracer time to initialize (parse config, start tracees, etc.)
sleep 2

# Verify tracer is still running
if ! kill -0 "$TRACER_PID" 2>/dev/null; then
    echo "Error: Tracer exited prematurely"
    wait "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

# Send 'c' command to start cmd_c mode
echo "c" > "$FIFO"

echo "cmd_c triggered, waiting for execution to start..."
sleep 1

# Verify tracer is still running after sending command
if ! kill -0 "$TRACER_PID" 2>/dev/null; then
    echo "Error: Tracer exited after cmd_c"
    exit 1
fi

echo "Starting perf record for ${PERF_DURATION} seconds on PID $TRACER_PID..."
echo "(Sampling at 200Hz with call graphs)"

# Run perf record attached to the tracer PID
PERF_DATA="$TMPDIR/perf.data"

if ! perf record -a -g --call-graph dwarf -F 200 -e cycles -g -p "$TRACER_PID" -o "$PERF_DATA" -- sleep $PERF_DURATION 2>&1; then
    echo "Warning: perf record may have issues"
fi

# Check if perf captured data
if [ ! -f "$PERF_DATA" ] || [ ! -s "$PERF_DATA" ]; then
    echo "Error: perf record failed to create output"
    # Kill tracer if still running
    kill "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

PERF_SIZE=$(stat -c%s "$PERF_DATA" 2>/dev/null || stat -f%z "$PERF_DATA" 2>/dev/null || echo 0)
echo "Perf data captured: $PERF_SIZE bytes"

if [ "$PERF_SIZE" -lt 100 ]; then
    echo "Error: perf data is too small"
    kill "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

echo "Generating flame graph..."

# Generate flame graph title
TITLE="tracer cmd_c - $CONFIG_BASENAME ($CONFIG_INFO) - $(date '+%Y-%m-%d %H:%M:%S')"

# Process perf data
echo "  - Converting perf data to script..."
if ! perf script -i "$PERF_DATA" > "$TMPDIR/perf.script" 2>/dev/null; then
    echo "Error: perf script failed"
    kill "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

SCRIPT_LINES=$(wc -l < "$TMPDIR/perf.script")
echo "  - Stack samples: $SCRIPT_LINES"

if [ "$SCRIPT_LINES" -lt 10 ]; then
    echo "Error: Not enough samples captured"
    kill "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

echo "  - Collapsing stacks..."
"$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$TMPDIR/perf.script" > "$TMPDIR/stacks.folded"

FOLDED_LINES=$(wc -l < "$TMPDIR/stacks.folded")
echo "  - Unique stacks: $FOLDED_LINES"

echo "  - Generating SVG..."
"$FLAMEGRAPH_DIR/flamegraph.pl" \
    --title "$TITLE" \
    --width 1600 \
    --height 24 \
    "$TMPDIR/stacks.folded" \
    > "$OUTPUT_SVG"

# Verify output
if [ ! -f "$OUTPUT_SVG" ] || [ ! -s "$OUTPUT_SVG" ]; then
    echo "Error: Failed to generate flame graph"
    kill "$TRACER_PID" 2>/dev/null || true
    exit 1
fi

# Cleanup: kill tracer
kill -SIGINT "$TRACER_PID" 2>/dev/null || true
wait "$TRACER_PID" 2>/dev/null || true
kill "$TRACER_PID" 2>/dev/null || true
wait "$TRACER_PID" 2>/dev/null || true

echo ""
echo "========================================"
echo "Flame graph generated successfully!"
echo "Output: $OUTPUT_SVG"
echo "Title: $TITLE"
echo "========================================"
ls -lh "$OUTPUT_SVG"
