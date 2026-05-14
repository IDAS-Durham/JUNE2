#!/bin/bash

# Script to clean rebuild JUNE2 locally
# Always performs a complete clean rebuild to ensure everything is up-to-date

set -e  # Exit on any error

echo "=== Complete clean rebuild ==="

# Save current directory
ORIGINAL_DIR=$(pwd)

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    echo "=== Creating build directory ==="
    mkdir build
fi

cd build

# Clean build directory
echo "=== Cleaning build directory ==="
rm -rf ./*

# Configure with CMake
echo "=== Running CMake configuration (Release, Tests Disabled) ==="
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DJUNE_MPI_DEBUG=OFF

# Build with parallel jobs
echo "=== Building with make -j8 ==="
make -j8

echo ""
echo "=== Build complete! ==="
echo "=== Build timestamp: $(date) ==="
echo ""

# Show binary info
if [ -f "disease_sim" ]; then
    echo "=== Binary information ==="
    ls -lh disease_sim
    echo ""
    echo "✓ Binary is fresh and all config files will be loaded at runtime"
fi

# Return to original directory
cd "$ORIGINAL_DIR"

echo ""
echo "REMINDER:"
echo "  • Config files (*.yaml) are loaded at RUNTIME, not compile time"
echo "  • If you only changed YAML files, a rebuild typically isn't needed"
echo "  • But running this script ensures everything is definitely up-to-date"
echo ""
