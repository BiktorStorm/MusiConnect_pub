#!/usr/bin/env bash
#
# setup.sh - Create a Python virtual environment, install dependencies
#            from requirements.txt, and activate the environment.
#
# Usage:
#   source ./setup.sh      (recommended - keeps the venv active in your shell)
#   ./setup.sh             (creates & installs, but activation ends when the script exits)
#
set -euo pipefail

# Directory of this script (works whether sourced or executed).
if [ -n "${BASH_SOURCE:-}" ]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
fi

VENV_DIR="${SCRIPT_DIR}/.venv"
REQUIREMENTS="${SCRIPT_DIR}/requirements.txt"

# Pick an available Python interpreter.
if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "Error: Python is not installed or not on PATH." >&2
    return 1 2>/dev/null || exit 1
fi

# Create the virtual environment if it doesn't already exist.
if [ ! -d "${VENV_DIR}" ]; then
    echo "Creating virtual environment in ${VENV_DIR}..."
    "${PYTHON}" -m venv "${VENV_DIR}"
else
    echo "Virtual environment already exists at ${VENV_DIR}."
fi

# Locate the activation script (Unix vs. Windows/Git-Bash layout).
if [ -f "${VENV_DIR}/bin/activate" ]; then
    ACTIVATE="${VENV_DIR}/bin/activate"
elif [ -f "${VENV_DIR}/Scripts/activate" ]; then
    ACTIVATE="${VENV_DIR}/Scripts/activate"
else
    echo "Error: could not find the activation script in ${VENV_DIR}." >&2
    return 1 2>/dev/null || exit 1
fi

# Activate the environment.
echo "Activating virtual environment..."
# shellcheck disable=SC1090
source "${ACTIVATE}"

# Upgrade pip and install dependencies.
echo "Upgrading pip..."
python -m pip install --upgrade pip

if [ -f "${REQUIREMENTS}" ]; then
    echo "Installing dependencies from ${REQUIREMENTS}..."
    python -m pip install -r "${REQUIREMENTS}"
else
    echo "Warning: ${REQUIREMENTS} not found; skipping dependency installation." >&2
fi

echo ""
echo "Setup complete. Virtual environment is active."
echo "If you ran this script directly (./setup.sh), run 'source ./setup.sh'"
echo "to keep the environment active in your current shell."
