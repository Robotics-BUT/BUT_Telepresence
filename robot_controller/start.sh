#!/bin/bash
#
# Startup script for Robot Controller Relay Service
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Robot Controller Relay Service - Startup Script"
echo "================================================"
echo

# Check if virtual environment exists
if [ ! -d "venv" ]; then
    echo "Creating virtual environment..."
    python3 -m venv venv
    echo
fi

# Activate virtual environment
echo "Activating virtual environment..."
source venv/bin/activate

# Install/update dependencies
echo "Installing dependencies..."
pip install -q --upgrade pip
pip install -q -r requirements.txt
echo

# Check if config file exists
if [ ! -f "config.yaml" ]; then
    echo "ERROR: config.yaml not found!"
    echo "Please create a config.yaml file with your robot configuration."
    echo "See the repository documentation for configuration options."
    echo
    exit 1
fi

# Run the service
echo "Starting Robot Controller Relay Service..."
echo "Press Ctrl+C to stop"
echo
cd ..
python -m robot_controller
