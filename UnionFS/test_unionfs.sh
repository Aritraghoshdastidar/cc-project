#!/bin/bash
set -euo pipefail

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "Starting Mini-UnionFS Test Suite..."

rm -rf "$TEST_DIR"
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

echo "base_only_content" > "$LOWER_DIR/base.txt"
echo "to_be_deleted" > "$LOWER_DIR/delete_me.txt"

"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 1

echo -n "Test 1: Layer Visibility... "
if grep -q "base_only_content" "$MOUNT_DIR/base.txt"; then
  echo -e "${GREEN}PASSED${NC}"
else
  echo -e "${RED}FAILED${NC}"
fi

echo -n "Test 2: Copy-on-Write... "
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null || true
MNT_COUNT=$(grep -c "modified_content" "$MOUNT_DIR/base.txt" 2>/dev/null || true)
UPPER_COUNT=$(grep -c "modified_content" "$UPPER_DIR/base.txt" 2>/dev/null || true)
LOWER_COUNT=$(grep -c "modified_content" "$LOWER_DIR/base.txt" 2>/dev/null || true)

MNT_COUNT=${MNT_COUNT:-0}
UPPER_COUNT=${UPPER_COUNT:-0}
LOWER_COUNT=${LOWER_COUNT:-0}

if [ "$MNT_COUNT" -eq 1 ] \
   && [ "$UPPER_COUNT" -eq 1 ] \
   && [ "$LOWER_COUNT" -eq 0 ]; then
  echo -e "${GREEN}PASSED${NC}"
else
  echo -e "${RED}FAILED${NC}"
fi

echo -n "Test 3: Whiteout mechanism... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null || true
if [ ! -f "$MOUNT_DIR/delete_me.txt" ] && [ -f "$LOWER_DIR/delete_me.txt" ] && [ -f "$UPPER_DIR/.wh.delete_me.txt" ]; then
  echo -e "${GREEN}PASSED${NC}"
else
  echo -e "${RED}FAILED${NC}"
fi

fusermount -u "$MOUNT_DIR" 2>/dev/null || umount "$MOUNT_DIR" 2>/dev/null || true
kill "$FUSE_PID" 2>/dev/null || true

rm -rf "$TEST_DIR"
echo "Test Suite Completed."
