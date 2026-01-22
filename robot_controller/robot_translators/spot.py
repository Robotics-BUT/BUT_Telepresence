"""
Spot robot protocol translator.

Translates VR client robot control messages to Spot robot protocol.
"""

import struct
from typing import Optional

from .base import RobotTranslator


class SpotTranslator(RobotTranslator):
    """
    Translator for Spot robot protocol.

    VR Client Format (21 bytes):
        [0x02] [linear_x (float)] [linear_y (float)] [angular (float)] [timestamp (uint64)]

    Spot Protocol Format (17 bytes):
        [0x23] [0x00] [0x01]
        [linear_x_scaled (float)] [linear_y_scaled (float)] [angular_scaled (float)]
        [0x00] [0x00]

    Scaling:
        - linear_x: multiplied by 0.5
        - linear_y: multiplied by -0.5 (inverted)
        - angular: multiplied by -0.5 (inverted)
    """

    # Protocol constants
    ODIN_HEADER = bytes([0x23, 0x00, 0x01])
    ODIN_FOOTER = bytes([0x00, 0x00])
    VR_PACKET_SIZE = 21
    ODIN_PACKET_SIZE = 17

    # Scaling factors
    LINEAR_X_SCALE = 0.5
    LINEAR_Y_SCALE = -0.5
    ANGULAR_SCALE = -0.5

    def __init__(self):
        """Initialize Spot translator."""
        super().__init__()
        self.logger.info("Spot translator initialized")

    def translate(self, vr_packet: bytes) -> Optional[bytes]:
        """
        Translate VR client packet to Spot protocol.

        Args:
            vr_packet: VR client robot control packet (21 bytes)

        Returns:
            Spot protocol packet (17 bytes), or None if translation failed
        """
        # Validate packet size
        if len(vr_packet) != self.VR_PACKET_SIZE:
            self.logger.error(f"Invalid VR packet size: {len(vr_packet)} bytes, expected {self.VR_PACKET_SIZE}")
            return None

        # Validate message type
        if vr_packet[0] != 0x02:
            self.logger.error(f"Invalid message type: 0x{vr_packet[0]:02x}, expected 0x02")
            return None

        try:
            # Parse VR packet (little-endian floats)
            offset = 1  # Skip message type byte

            linear_x = struct.unpack('<f', vr_packet[offset:offset+4])[0]
            offset += 4

            linear_y = struct.unpack('<f', vr_packet[offset:offset+4])[0]
            offset += 4

            angular = struct.unpack('<f', vr_packet[offset:offset+4])[0]
            # Note: timestamp at offset+4 to offset+12 is not used in Spot protocol

            # Apply scaling
            linear_x_scaled = linear_x * self.LINEAR_X_SCALE
            linear_y_scaled = linear_y * self.LINEAR_Y_SCALE
            angular_scaled = angular * self.ANGULAR_SCALE

            # Build Spot packet
            odin_packet = bytearray()
            odin_packet.extend(self.ODIN_HEADER)
            odin_packet.extend(struct.pack('<f', linear_x_scaled))
            odin_packet.extend(struct.pack('<f', linear_y_scaled))
            odin_packet.extend(struct.pack('<f', angular_scaled))
            odin_packet.extend(self.ODIN_FOOTER)

            self.logger.debug(
                f"Translated: VR[x={linear_x:.3f}, y={linear_y:.3f}, a={angular:.3f}] -> "
                f"Spot[x={linear_x_scaled:.3f}, y={linear_y_scaled:.3f}, a={angular_scaled:.3f}]"
            )

            return bytes(odin_packet)

        except struct.error as e:
            self.logger.error(f"Failed to parse VR packet: {e}")
            return None
        except Exception as e:
            self.logger.error(f"Translation error: {e}")
            return None

    def get_name(self) -> str:
        """Get translator name."""
        return "Spot Robot Translator"
