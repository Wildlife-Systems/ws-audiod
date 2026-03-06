#!/bin/bash
# Integration test: exercises all daemon features simultaneously

DAEMON=~/ws-audiod/build/ws-audiod
SOCK=/run/ws-audiod/control.sock
BLOCKS=/var/ws/audiod/blocks
CLIPS=/var/ws/audiod/clips
SHM=/dev/shm/ws_audiod_samples
PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() {
    [ -n "$PID" ] && kill $PID 2>/dev/null
    wait $PID 2>/dev/null
}
trap cleanup EXIT

echo "=== ws-audiod integration test ==="

# Kill any stale daemon (but not this script)
pkill -x ws-audiod 2>/dev/null || true
sleep 1

# Cleanup files
rm -f $BLOCKS/*.wav $CLIPS/*.wav $CLIPS/*.flac
sudo mkdir -p /run/ws-audiod && sudo chown ed:ed /run/ws-audiod

# Start daemon with all features
echo "Starting daemon..."
$DAEMON -D plughw:1 -b 32 -C 1 -r 48000 -B 5 -S -d >/dev/null 2>&1 &
PID=$!
sleep 2

# --- 1. Daemon running ---
echo ""
echo "--- 1. Daemon running ---"
if kill -0 $PID 2>/dev/null; then pass "daemon process alive"; else fail "daemon process alive"; fi

# --- 2. Control socket ---
echo ""
echo "--- 2. Control socket ---"
STATUS=$(echo "GET STATUS" | socat - UNIX-CONNECT:$SOCK 2>/dev/null)
echo "  STATUS: $STATUS"
if echo "$STATUS" | grep -q '"ok":true'; then pass "status returns ok"; else fail "status returns ok"; fi
if echo "$STATUS" | grep -q '"running":true'; then pass "status shows running"; else fail "status shows running"; fi
if echo "$STATUS" | grep -q 'plughw:1'; then pass "status shows device"; else fail "status shows device"; fi

# --- 3. Level metering ---
echo ""
echo "--- 3. Level metering ---"
LEVELS=$(echo "GET LEVELS" | socat - UNIX-CONNECT:$SOCK 2>/dev/null)
echo "  LEVELS: $LEVELS"
if echo "$LEVELS" | grep -q '"ok":true'; then pass "levels returns ok"; else fail "levels returns ok"; fi
if echo "$LEVELS" | grep -q '"peak"'; then pass "levels has peak"; else fail "levels has peak"; fi
if echo "$LEVELS" | grep -q '"rms"'; then pass "levels has rms"; else fail "levels has rms"; fi

# --- 4. Shared memory (SHM) ---
echo ""
echo "--- 4. Shared memory (SHM) ---"
if [ -f $SHM ]; then pass "shm file exists"; else fail "shm file exists"; fi
if [ -f $SHM ]; then
    SHM_SIZE=$(stat -c%s $SHM)
    echo "  SHM size: $SHM_SIZE bytes"
    if [ $SHM_SIZE -gt 64 ]; then pass "shm has data (>64 bytes header)"; else fail "shm has data"; fi
fi

# --- 5. Block recording ---
echo ""
echo "--- 5. Block recording ---"
echo "  Waiting for first block (5s)..."
sleep 4
BLOCK_COUNT=$(ls -1 $BLOCKS/*.wav 2>/dev/null | wc -l)
echo "  Block files: $BLOCK_COUNT"
if [ $BLOCK_COUNT -ge 1 ]; then pass "at least 1 block recorded"; else fail "at least 1 block recorded"; fi
if [ $BLOCK_COUNT -ge 1 ]; then
    FIRST_BLOCK=$(ls -1 $BLOCKS/*.wav | head -1)
    BLOCK_INFO=$(file "$FIRST_BLOCK")
    echo "  $BLOCK_INFO"
    if echo "$BLOCK_INFO" | grep -q "32 bit"; then pass "block is 32-bit WAV"; else fail "block is 32-bit WAV"; fi
    if echo "$BLOCK_INFO" | grep -q "mono"; then pass "block is mono"; else fail "block is mono"; fi
    if echo "$BLOCK_INFO" | grep -q "48000"; then pass "block is 48kHz"; else fail "block is 48kHz"; fi
fi

# --- 6. Clip extraction ---
echo ""
echo "--- 6. Clip extraction ---"
CLIP_RESP=$(echo "CLIP -3 0" | socat - UNIX-CONNECT:$SOCK 2>/dev/null)
echo "  CLIP response: $CLIP_RESP"
if echo "$CLIP_RESP" | grep -q '"ok":true'; then pass "clip returns ok"; else fail "clip returns ok"; fi
if echo "$CLIP_RESP" | grep -q '"path"'; then pass "clip returns path"; else fail "clip returns path"; fi
sleep 1
CLIP_COUNT=$(ls -1 $CLIPS/*.wav $CLIPS/*.flac 2>/dev/null | wc -l)
echo "  Clip files: $CLIP_COUNT"
if [ $CLIP_COUNT -ge 1 ]; then pass "clip file created"; else fail "clip file created"; fi
if [ $CLIP_COUNT -ge 1 ]; then
    CLIP_FILE=$(ls -1 $CLIPS/*.wav $CLIPS/*.flac 2>/dev/null | head -1)
    CLIP_INFO=$(file "$CLIP_FILE")
    echo "  $CLIP_INFO"
    if echo "$CLIP_INFO" | grep -q "32 bit"; then pass "clip is 32-bit"; else fail "clip is 32-bit"; fi
fi

# --- 7. Audio consumer (SHM read) ---
echo ""
echo "--- 7. Audio consumer (SHM read) ---"
if [ -f ~/ws-audiod/build/audio_consumer ]; then
    CONSUMER_OUT=$(timeout 2 ~/ws-audiod/build/audio_consumer 2>&1)
    echo "$CONSUMER_OUT" | head -8
    if echo "$CONSUMER_OUT" | grep -q "Connected"; then pass "audio consumer connects"; else fail "audio consumer connects"; fi
    if echo "$CONSUMER_OUT" | grep -q "Consumed"; then pass "consumer reads periods"; else fail "consumer reads periods"; fi
else
    echo "  SKIP: audio_consumer not built"
fi

# --- 8. Status after all operations ---
echo ""
echo "--- 8. Status after all operations ---"
FINAL=$(echo "GET STATUS" | socat - UNIX-CONNECT:$SOCK 2>/dev/null)
echo "  FINAL STATUS: $FINAL"
if echo "$FINAL" | grep -q '"running":true'; then pass "still running after all tests"; else fail "still running"; fi
FRAMES=$(echo "$FINAL" | grep -o '"frames":[0-9]*' | head -1 | grep -o '[0-9]*')
echo "  Total frames captured: $FRAMES"
if [ "${FRAMES:-0}" -gt 0 ]; then pass "frames captured > 0"; else fail "frames captured > 0"; fi

# --- 9. Clean shutdown ---
echo ""
echo "--- 9. Clean shutdown ---"
OLD_PID=$PID
kill $PID 2>/dev/null
wait $PID 2>/dev/null
PID=""
sleep 1
if ! kill -0 $OLD_PID 2>/dev/null; then pass "daemon exited cleanly"; else fail "daemon exited cleanly"; fi
if [ ! -e $SOCK ]; then pass "socket cleaned up"; else fail "socket cleaned up"; fi

echo ""
echo "========================================="
echo "  PASSED: $PASS  FAILED: $FAIL"
echo "========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1
