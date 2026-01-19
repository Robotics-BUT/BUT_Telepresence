"""
TG Drives servo translator implementation.

Translates simple azimuth/elevation messages to TG Drives GT protocol.

Incoming format (17 bytes):
    Byte 0:     0x01 (message type)
    Bytes 1-4:  azimuth (float32, little-endian) in radians
    Bytes 5-8:  elevation (float32, little-endian) in radians
    Bytes 9-16: timestamp (uint64, little-endian)

Outgoing format (GT protocol):
    Bytes 0-1:  0x47, 0x54 ("GT" magic bytes)
    Then multiple operations for azimuth/elevation angle, speed, and enable
"""

import logging
import socket
import struct
from typing import Tuple, Optional, Dict
import math

from .base import ServoTranslator


class TGDrivesTranslator(ServoTranslator):
    """
    Translator for TG Drives servo driver.

    Converts simple azimuth/elevation radians to the complex GT protocol
    with operations, message groups, and elements.
    """

    # GT Protocol constants
    IDENTIFIER_1 = 0x47  # 'G'
    IDENTIFIER_2 = 0x54  # 'T'

    # Operations
    WRITE = 0x02
    WRITE_CONTINUOUS = 0x04

    # Message Groups
    ENABLE_ELEVATION = 0x11
    ENABLE_AZIMUTH = 0x12
    ELEVATION = 0x19
    AZIMUTH = 0x1A

    # Message Elements
    ENABLE = 0x00
    ANGLE = 0x04
    SPEED = 0x07
    MODE = 0x09

    def __init__(self, servo_ip: str, servo_port: int, timeout: float,
                 azimuth_min: int = -180000, azimuth_max: int = 180000,
                 elevation_min: int = -90000, elevation_max: int = 90000,
                 speed_max: int = 1000000, speed_multiplier: float = 0.0,
                 filter_alpha: float = 0.15, swap_axes: bool = False,
                 invert_azimuth: bool = False, invert_elevation: bool = False):
        """
        Initialize TG Drives translator.

        Args:
            servo_ip: IP address of the TG Drives servo driver
            servo_port: Port of the TG Drives servo driver
            timeout: Response timeout in seconds
            azimuth_min: Minimum azimuth value in motor units
            azimuth_max: Maximum azimuth value in motor units
            elevation_min: Minimum elevation value in motor units
            elevation_max: Maximum elevation value in motor units
            speed_max: Maximum speed value for servos
            speed_multiplier: Speed multiplier for accelerated movement
            filter_alpha: Low-pass filter alpha (0-1, higher = less filtering)
            swap_axes: Whether to swap azimuth and elevation axes
            invert_azimuth: Whether to invert azimuth direction
            invert_elevation: Whether to invert elevation direction
        """
        super().__init__(servo_ip, servo_port, timeout)
        self.logger = logging.getLogger(__name__)

        # Movement range configuration
        self.azimuth_min = azimuth_min
        self.azimuth_max = azimuth_max
        self.elevation_min = elevation_min
        self.elevation_max = elevation_max
        self.speed_max = speed_max
        self.speed_multiplier = speed_multiplier
        self.swap_axes = swap_axes
        self.invert_azimuth = invert_azimuth
        self.invert_elevation = invert_elevation

        # Filtering
        self.filter_alpha = filter_alpha
        self.azimuth_filtered = 0
        self.elevation_filtered = 0

        # Out-of-order packet guard
        self.last_timestamp = 0

    def initialize(self) -> bool:
        """
        Initialize UDP socket and set servo mode.

        Returns:
            True if initialization succeeded, False otherwise
        """
        try:
            # Create UDP socket
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.socket.bind(("0.0.0.0", 0))
            self.socket.settimeout(self.timeout)

            self.logger.info(f"TG Drives translator initialized: {self.servo_ip}:{self.servo_port}")

            # Set mode to position control on both axes
            self._set_mode()

            return True
        except Exception as e:
            self.logger.error(f"Failed to initialize TG Drives translator: {e}")
            return False

    def _set_mode(self):
        """Set servo mode to position control."""
        mode_buffer = bytearray([
            self.IDENTIFIER_1, self.IDENTIFIER_2,
            self.WRITE,
            self.AZIMUTH, self.MODE,
            0x01, 0x00, 0x00, 0x00,
            self.WRITE,
            self.ELEVATION, self.MODE,
            0x01, 0x00, 0x00, 0x00
        ])

        try:
            self.socket.sendto(mode_buffer, (self.servo_ip, self.servo_port))
            self.logger.info("Set servo mode to position control")
        except Exception as e:
            self.logger.warning(f"Failed to set servo mode: {e}")

    def translate_and_forward(self, data: bytes, client_addr: Tuple[str, int]) -> Optional[bytes]:
        """
        Translate azimuth/elevation message to GT protocol and forward.

        Args:
            data: Raw head pose command (0x01 + azimuth + elevation + timestamp)
            client_addr: Address of the client (IP, port)

        Returns:
            Response bytes from servo driver, or None on error
        """
        if not self.socket:
            self.logger.error("Socket not initialized")
            return None

        # Validate packet length (21 bytes expected)
        if len(data) != 21:
            self.logger.warning(f"Invalid packet length: {len(data)} bytes, expected 21")
            return None

        # Verify message type
        if data[0] != 0x01:
            self.logger.warning(f"Unexpected message type: 0x{data[0]:02x}, expected 0x01")
            return None

        # Parse azimuth and elevation (floats in radians)
        try:
            azimuth_rad = struct.unpack('<f', data[1:5])[0]
            elevation_rad = struct.unpack('<f', data[5:9])[0]
            speed_motor = struct.unpack('<f', data[9:13])[0]
            timestamp = struct.unpack('<Q', data[13:21])[0]
        except struct.error as e:
            self.logger.error(f"Failed to parse message: {e}")
            return None

        # Guard against out-of-order packets
        if timestamp <= self.last_timestamp:
            self.logger.warning(f"Out-of-order packet dropped: ts={timestamp}, last_ts={self.last_timestamp}")
            return None

        self.last_timestamp = timestamp

        self.logger.debug(f"Received: azimuth={azimuth_rad:.3f} rad, elevation={elevation_rad:.3f} rad, speed={speed_motor}, ts={timestamp}")

        # Swap axes if configured
        if self.swap_axes:
            azimuth_rad, elevation_rad = elevation_rad, azimuth_rad
            self.logger.debug(f"Axes swapped: azimuth={azimuth_rad:.3f} rad, elevation={elevation_rad:.3f} rad")

        # Invert axis directions if configured
        if self.invert_azimuth:
            azimuth_rad = -azimuth_rad
            self.logger.debug(f"Azimuth inverted: {azimuth_rad:.3f} rad")

        if self.invert_elevation:
            elevation_rad = -elevation_rad
            self.logger.debug(f"Elevation inverted: {elevation_rad:.3f} rad")

        # Convert radians to motor units
        azimuth_motor, elevation_motor = self._convert_to_motor_units(azimuth_rad, elevation_rad)

        # Build GT protocol message
        gt_message = self._build_gt_message(azimuth_motor, elevation_motor, int(speed_motor))

        # Send to servo driver
        try:
            self.socket.sendto(gt_message, (self.servo_ip, self.servo_port))
            self.logger.debug(f"Sent GT message: az={azimuth_motor}, el={elevation_motor}")
        except Exception as e:
            self.logger.error(f"Failed to send to servo driver: {e}")
            return None

        # Wait for response (optional, based on original code it seems responses were disabled)
        # The original code has waitForResponse commented out in the while loop
        # For now, we'll skip waiting for response to match that behavior
        return None

    def _convert_to_motor_units(self, azimuth_rad: float, elevation_rad: float) -> Tuple[int, int]:
        """
        Convert azimuth/elevation from radians to motor units.

        Args:
            azimuth_rad: Azimuth in radians
            elevation_rad: Elevation in radians

        Returns:
            Tuple of (azimuth_motor, elevation_motor) in motor units
        """
        # Calculate range centers and max sides
        azimuth_max_side = (self.azimuth_max - self.azimuth_min) // 2
        azimuth_center = self.azimuth_max - azimuth_max_side

        elevation_max_side = (self.elevation_max - self.elevation_min) // 2
        elevation_center = self.elevation_max - elevation_max_side

        # Convert radians to motor units
        azimuth_motor = int(((azimuth_rad * 2.0) / math.pi) * azimuth_max_side + azimuth_center)
        elevation_motor = int(((-elevation_rad * 2.0) / math.pi) * elevation_max_side + elevation_center)

        # Apply speed multiplier
        azimuth_motor += int((azimuth_motor - azimuth_center) * self.speed_multiplier)
        elevation_motor += int((elevation_motor - elevation_center) * self.speed_multiplier)

        # Apply low-pass filter
        self.azimuth_filtered = int(self.azimuth_filtered * (1.0 - self.filter_alpha) + azimuth_motor * self.filter_alpha)
        self.elevation_filtered = int(self.elevation_filtered * (1.0 - self.filter_alpha) + elevation_motor * self.filter_alpha)

        # Clamp to limits
        self.azimuth_filtered = max(self.azimuth_min, min(self.azimuth_max, self.azimuth_filtered))
        self.elevation_filtered = max(self.elevation_min, min(self.elevation_max, self.elevation_filtered))

        return self.azimuth_filtered, self.elevation_filtered

    def _build_gt_message(self, azimuth: int, elevation: int, speed: int) -> bytes:
        """
        Build GT protocol message for servo control.

        Args:
            azimuth: Azimuth value in motor units
            elevation: Elevation value in motor units
            speed: Speed value in motor units

        Returns:
            Complete GT protocol message bytes
        """
        # Calculate revolution counts (for multi-turn servos)
        az_revol = -1 if azimuth < 0 else 0
        el_revol = -1 if elevation < 0 else 0

        # Serialize values as little-endian int32
        az_angle_bytes = struct.pack('<i', azimuth)
        az_revol_bytes = struct.pack('<i', az_revol)
        el_angle_bytes = struct.pack('<i', elevation)
        el_revol_bytes = struct.pack('<i', el_revol)
        speed_bytes = struct.pack('<i', speed)

        # Build the complete message
        message = bytearray([
            self.IDENTIFIER_1, self.IDENTIFIER_2,

            # Azimuth angle (WRITE_CONTINUOUS)
            self.WRITE_CONTINUOUS,
            self.AZIMUTH, self.ANGLE,
            0x02,  # Number of values (angle + revolution)
        ])
        message.extend(az_angle_bytes)
        message.extend(az_revol_bytes)

        message.extend([
            # Elevation angle (WRITE_CONTINUOUS)
            self.WRITE_CONTINUOUS,
            self.ELEVATION, self.ANGLE,
            0x02,  # Number of values (angle + revolution)
        ])
        message.extend(el_angle_bytes)
        message.extend(el_revol_bytes)

        message.extend([
            # Azimuth speed
            self.WRITE,
            self.AZIMUTH, self.SPEED,
        ])
        message.extend(speed_bytes)

        message.extend([
            # Elevation speed
            self.WRITE,
            self.ELEVATION, self.SPEED,
        ])
        message.extend(speed_bytes)

        message.extend([
            # Enable azimuth
            self.WRITE,
            self.ENABLE_AZIMUTH, self.ENABLE,
            0x01, 0x00, 0x00, 0x00,

            # Enable elevation
            self.WRITE,
            self.ENABLE_ELEVATION, self.ENABLE,
            0x01, 0x00, 0x00, 0x00,
        ])

        return bytes(message)

    def close(self):
        """Close the UDP socket."""
        if self.socket:
            try:
                self.socket.close()
                self.logger.info("TG Drives translator closed")
            except Exception as e:
                self.logger.error(f"Error closing socket: {e}")
            finally:
                self.socket = None
