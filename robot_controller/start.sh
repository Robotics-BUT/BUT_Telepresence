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

# Check if telemetry is enabled in config.yaml
TELEMETRY_ENABLED=$(python3 -c "import yaml; print(yaml.safe_load(open('config.yaml')).get('telemetry',{}).get('enabled', False))" 2>/dev/null || echo "False")

if [ "$TELEMETRY_ENABLED" = "True" ]; then
    echo "Telemetry enabled: waiting for influxdb3.service on port 8181..."
    for i in {1..30}; do
        if nc -z localhost 8181 2>/dev/null || curl -s http://localhost:8181/health > /dev/null 2>&1; then
            echo "InfluxDB is ready!"
            break
        fi
        if [ "$i" -eq 30 ]; then
            echo "WARNING: InfluxDB (influxdb3.service) not ready after 30s; starting relay anyway."
        fi
        sleep 1
    done
else
    echo "Telemetry disabled: skipping InfluxDB."
fi

# Run the service
echo "Starting Robot Controller Relay Service..."
echo "Press Ctrl+C to stop"
echo
cd ..
python -m robot_controller
