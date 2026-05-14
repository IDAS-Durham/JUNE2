#!/bin/bash
# Sets up local git hooks for the JUNE 2 project.

echo "Setting up JUNE 2 git hooks..."

# Ensure we are in the project root
cd "$(dirname "$0")/.."

# Path to our custom hooks directory
HOOKS_DIR=".githooks"

# Check if the hooks directory exists
if [ ! -d "$HOOKS_DIR" ]; then
    echo "Error: Directory '$HOOKS_DIR' does not exist."
    exit 1
fi

# Make hooks executable
echo "Making hooks executable..."
chmod +x "$HOOKS_DIR"/*

# Configure git to use our hooks directory
echo "Configuring git to use '$HOOKS_DIR'..."
git config core.hooksPath "$HOOKS_DIR"

echo "✅ Git hooks successfully configured!"
echo "The pre-push hook will now run automatically before every 'git push'."
