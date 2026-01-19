#!/bin/bash
# Unix/Linux build script for HTTPServer
# Separates compilation and linking phases for better control

# Configuration variables
COMPILER=gcc
CFLAGS="${CFLAGS:--std=c99 -Wall -Wextra -O2}"
OUTPUT_DIR="build/objs"
EXECUTABLE="httpserver"
LINK_FLAGS=""

# Detect platform and set platform-specific flags
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    LINK_FLAGS="-lz"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    LINK_FLAGS="-lz"
else
    echo "[WARNING] Unknown platform: $OSTYPE"
    LINK_FLAGS="-lz"
fi

# Create output directory for object files
mkdir -p "$OUTPUT_DIR"

echo "[INFO] Compiling C source files..."

# Compile each source file to object files
find src -name "*.c" | while read SRCFILE; do
    OBJFILE="$OUTPUT_DIR/$(basename "$SRCFILE" .c).o"
    echo "Compiling $(basename "$SRCFILE")..."
    $COMPILER $CFLAGS -c "$SRCFILE" -o "$OBJFILE"
    if [ $? -ne 0 ]; then
        echo "[ERROR] Compilation failed for $(basename "$SRCFILE")"
        exit 1
    fi
done

echo "[INFO] Linking object files..."

# Link all object files
OBJ_FILES=$(find "$OUTPUT_DIR" -name "*.o" -type f)
$COMPILER $OBJ_FILES $LINK_FLAGS -o "$EXECUTABLE"

if [ $? -eq 0 ]; then
    echo "[SUCCESS] Build complete: $EXECUTABLE"
else
    echo "[ERROR] Linking failed"
    exit 1
fi