# Robot Controller - UDP Relay Service

A UDP relay service for routing robot control commands to different subsystems. This service receives commands via UDP and routes them to the appropriate destination:
- **Head pose messages** (0x01 prefix) → Servo driver via translator
- **Robot control messages** (0x02 prefix) → Robot controller

## Architecture

The service is designed with abstraction in mind to support different robot types with different proprietary servo drivers.

```
┌──────────────────┐
│  External        │
│  Controller      │ (Sends UDP commands)
└────────┬─────────┘
         │ UDP :32115
         ▼
┌────────────────────────────────────┐
│   Robot Controller Relay Service   │
│                                    │
│  ┌──────────────────────────────┐ │
│  │   Message Detector           │ │
│  │   (0x01 vs 0x02)             │ │
│  └────────┬──────────┬──────────┘ │
│           │          │             │
│   0x01    │          │   0x02      │
│           ▼          ▼             │
│  ┌────────────┐  ┌──────────────┐ │
│  │   Servo    │  │    Robot     │ │
│  │ Translator │  │   Forward    │ │
│  └─────┬──────┘  └──────┬───────┘ │
└────────┼─────────────────┼─────────┘
         │                 │
         ▼                 ▼
   ┌─────────────┐   ┌──────────┐
   │Servo Driver │   │  Robot   │
   │192.168.1.150│   │10.0.31.11│
   │    :502     │   │   :5555  │
   └─────────────┘   └──────────┘
```

### Key Components

1. **MessageDetector** (`protocol.py`) - Detects message type based on prefix
2. **ServoTranslator** (`servo_translators/`) - Abstracts servo driver communication
   - `base.py` - Abstract interface for all translators
   - `tg_drives.py` - TG Drives specific implementation
3. **UDPRelayService** (`relay_service.py`) - Main service orchestrator
4. **Configuration** (`config.py`) - YAML-based configuration management

## Installation

### Prerequisites

- Python 3.7+
- pip

### Quick Start

```bash
cd robot_controller
./start.sh
```

The start script will:
1. Create a virtual environment if needed
2. Install dependencies
3. Verify `config.yaml` exists
4. Start the service

### Manual Installation

```bash
# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Edit configuration
nano config.yaml

# Run the service
python -m relay_service
```

## Configuration

Configuration is provided through the mandatory `config.yaml` file.

## Protocols

### Head Pose Protocol (0x01)

Messages starting with `0x01` are head pose/servo control commands.

**Structure (17 bytes):**
```
Byte 0:     0x01 (message type)
Bytes 1-4:  azimuth (float32, little-endian) in radians
Bytes 5-8:  elevation (float32, little-endian) in radians
Bytes 9-16: timestamp (uint64, little-endian) in microseconds
```

**Processing:**
1. Message detector identifies 0x01 prefix
2. Servo translator receives full 17-byte message
3. Translator parses azimuth, elevation, and timestamp
4. Out-of-order guard: Packets with timestamp ≤ last_timestamp are dropped
5. Translator converts radians to motor units specific to servo driver
6. Translator applies low-pass filtering for smooth movement
7. Translator builds servo-driver-specific protocol message
8. Message is sent to servo driver

### Robot Control Protocol (0x02)

Messages starting with `0x02` are robot movement commands.

**Structure (21 bytes):**
```
Byte 0:     0x02 (message type)
Bytes 1-4:  linearX (float32, little-endian) in m/s
Bytes 5-8:  linearY (float32, little-endian) in m/s
Bytes 9-12: angular (float32, little-endian) in rad/s
Bytes 13-20: timestamp (uint64, little-endian) in microseconds
```

**Processing:**
1. Message detector identifies 0x02 prefix
2. Full message (including 0x02) is forwarded directly to robot controller
3. No translation or filtering applied

## Servo Translators

The servo translation layer is abstracted to support different robot types with different proprietary servo drivers. Each robot can have its own translator implementation.

### TG Drives Translator

The included `TGDrivesTranslator` converts simple azimuth/elevation commands to the TG Drives GT protocol.

**Features:**
- Converts radians to motor units using configurable range mapping
- Applies low-pass filtering (configurable alpha) for smooth movement
- Guards against out-of-order packets using timestamps
- Clamps values to configured min/max limits
- Builds complex GT protocol messages with operations, groups, and elements

**Out-of-order Protection:**
- Tracks last processed timestamp
- Drops packets with timestamp ≤ last_timestamp
- Logs warning when packets are dropped
- Prevents jerky servo movement from network packet reordering

### Creating a New Translator

1. Create a new file in `servo_translators/`, e.g., `my_robot.py`
2. Inherit from `ServoTranslator` base class:

```python
from .base import ServoTranslator
from typing import Optional, Tuple

class MyRobotTranslator(ServoTranslator):
    def initialize(self) -> bool:
        """Initialize connection to servo driver."""
        # Create socket, establish connection, etc.
        return True

    def translate_and_forward(self, data: bytes, client_addr: Tuple[str, int]) -> Optional[bytes]:
        """
        Translate and forward head pose command.

        Args:
            data: 17-byte head pose message (0x01 + azimuth + elevation + timestamp)
            client_addr: Address of the client

        Returns:
            Response bytes from servo driver, or None
        """
        # Parse the 17-byte message
        # Convert to your servo driver's format
        # Send to servo driver
        # Return response if needed
        return None

    def close(self):
        """Clean up resources."""
        # Close sockets, connections, etc.
        pass
```

3. Register in `servo_translators/__init__.py`:

```python
from .my_robot import MyRobotTranslator
__all__ = ['ServoTranslator', 'TGDrivesTranslator', 'MyRobotTranslator']
```

4. Update `relay_service.py` to support your translator:

```python
def _create_servo_translator(self) -> ServoTranslator:
    translator_type = self.config.servo_translator.lower()

    if translator_type == 'tg_drives':
        return TGDrivesTranslator(...)
    elif translator_type == 'my_robot':
        return MyRobotTranslator(...)
    else:
        raise RelayServiceError(f"Unknown servo translator type: {translator_type}")
```

5. Update configuration to use your translator:

```yaml
network:
  servo:
    translator: "my_robot"
```

## Running the Service

### Development Mode

```bash
# Run with debug logging (edit config.yaml first)
python -m relay_service
```

To enable debug logging, edit `config.yaml`:
```yaml
logging:
  level: "DEBUG"
```

### Production Mode

```bash
# Run with info logging (default)
python -m relay_service

# Or use the startup script
./start.sh
```

### As a systemd Service

Create `/etc/systemd/system/robot-relay.service`:

```ini
[Unit]
Description=Robot Controller Relay Service
After=network.target

[Service]
Type=simple
User=robot
WorkingDirectory=/path/to/Telepresence-Streaming-Driver/robot_controller
ExecStart=/path/to/Telepresence-Streaming-Driver/robot_controller/venv/bin/python -m relay_service
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable robot-relay
sudo systemctl start robot-relay
sudo systemctl status robot-relay
```

## Monitoring & Logs

### Log Levels

- **DEBUG**: Detailed packet information, timestamps, motor values
- **INFO**: Service lifecycle events (start, stop, configuration)
- **WARNING**: Out-of-order packets, malformed packets, recoverable errors
- **ERROR**: Socket failures, initialization failures, critical errors

### Console Logging (Default)

By default, logs are printed to stdout:

```bash
python -m relay_service
```

### File Logging

To save logs to a file, edit `config.yaml`:

```yaml
logging:
  level: "INFO"
  file: "/var/log/robot_relay.log"
```

Both console and file logging will be active when a file is specified.

### View Logs

```bash
# If using systemd
sudo journalctl -u robot-relay -f

# If logging to file
tail -f /var/log/robot_relay.log

# If running manually
# Logs appear in terminal
```

## Troubleshooting

### Port Already in Use

```
OSError: [Errno 98] Address already in use
```

**Solution:** Another process is using port 32115. Either stop that process or change the port in configuration.

```bash
# Find process using the port
sudo lsof -i :32115

# Kill the process
sudo kill -9 <PID>
```

### Servo Response Timeout

```
WARNING - Servo driver response timeout
```

**Possible causes:**
- Servo driver is not running
- Incorrect servo IP/port configuration
- Network connectivity issues
- Servo driver is overloaded

**Solution:** Check servo driver status and network connectivity.

### Out-of-order Packets

```
WARNING - Out-of-order packet dropped: ts=1234567890, last_ts=1234567900
```

**Possible causes:**
- Network packet reordering (normal on UDP)
- Multiple clients sending to same service

**Solution:** This is usually normal behavior. The service automatically drops these packets to prevent jerky servo movement. If frequent, investigate network quality.

### Invalid Packet Length

```
WARNING - Invalid packet length: 15 bytes, expected 17
```

**Possible causes:**
- Client sending malformed packets
- Protocol version mismatch
- Data corruption

**Solution:** Check client implementation and verify protocol format matches documentation.

### Unknown Message Type

```
WARNING - Unknown message type from X.X.X.X:XXXXX, dropping
```

**Possible causes:**
- Client sending unsupported message type
- Data corruption
- Wrong protocol version

**Solution:** Verify client is sending 0x01 or 0x02 as first byte.

## Security Considerations

- **No authentication:** The service does not authenticate incoming commands
- **No encryption:** All communication is in plaintext UDP
- **Network isolation:** Should be run on a trusted network
- **Firewall:** Consider restricting ingest port to known client IPs

For production use in untrusted environments, consider:
- Adding authentication tokens to message protocol
- Using encrypted channels (VPN, IPSec)
- Implementing rate limiting
- Adding command validation and bounds checking
- Running service with minimal privileges

## License

See main project LICENSE file.

## Support

For issues, feature requests, or questions, please open an issue in the main repository.
