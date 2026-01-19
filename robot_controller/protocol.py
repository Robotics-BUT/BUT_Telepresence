"""
Message type detection for robot communication protocol.

This module only handles identifying the message type (head pose, robot control, debug info).
The actual servo protocol translation is handled by servo_translators.
"""

import logging
from enum import Enum


class MessageType(Enum):
    """Type of incoming message."""
    HEAD_POSE = "head_pose"  # Servo/head control
    ROBOT_CONTROL = "robot_control"  # Robot movement
    DEBUG_INFO = "debug_info"
    UNKNOWN = "unknown"


class MessageDetector:
    """
    Detects message type based on protocol prefix.

    Protocol:
    - Head pose messages (servo control): Start with 0x01
    - Robot control messages: Start with 0x02
    - Debug info messages: Start with 0x03
    """

    # Protocol constants
    HEAD_POSE_PREFIX = 0x01
    ROBOT_CONTROL_PREFIX = 0x02
    DEBUG_INFO_PREFIX = 0x03

    def __init__(self):
        self.logger = logging.getLogger(__name__)

    def detect_message_type(self, data: bytes) -> MessageType:
        """
        Detect message type based on prefix byte.

        Args:
            data: Raw UDP packet data

        Returns:
            MessageType enum value
        """
        if len(data) < 1:
            self.logger.warning("Empty packet received")
            return MessageType.UNKNOWN

        prefix = data[0]

        # Check for head pose message (servo control)
        if prefix == self.HEAD_POSE_PREFIX:
            return MessageType.HEAD_POSE

        # Check for robot control message
        elif prefix == self.ROBOT_CONTROL_PREFIX:
            return MessageType.ROBOT_CONTROL

        elif prefix == self.DEBUG_INFO_PREFIX:
            return MessageType.DEBUG_INFO

        # Unknown message type
        else:
            self.logger.warning(f"Unknown message type with prefix: 0x{prefix:02x}")
            return MessageType.UNKNOWN
