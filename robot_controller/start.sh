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

echo "Cleaning WAL from previous unclean shutdown..."
rm -rf ~/.influxdb/but_telepresence_telemetry/wal/*

echo "Serving the database..."
influxdb3 serve --without-auth --node-id=but_telepresence_telemetry &
INFLUX_PID=$!

# Wait for InfluxDB to be ready
echo "Waiting for InfluxDB to be ready..."
for i in {1..30}; do
    # Check if process is still alive
    if ! kill -0 $INFLUX_PID 2>/dev/null; then
        echo "ERROR: InfluxDB process died during startup!"
        echo "Check logs at ~/.influxdb/but_telepresence_telemetry/ for errors"
        exit 1
    fi

    # Check if port 8181 is accepting connections
    if nc -z localhost 8181 2>/dev/null || curl -s http://localhost:8181/health > /dev/null 2>&1; then
        echo "InfluxDB is ready!"
        break
    fi

    if [ $i -eq 30 ]; then
        echo "ERROR: InfluxDB failed to start within 30 seconds"
        kill $INFLUX_PID 2>/dev/null
        exit 1
    fi
    sleep 1
done

# Run the service
echo "Starting Robot Controller Relay Service..."
echo "Press Ctrl+C to stop"
echo
cd ..
python -m robot_controller
