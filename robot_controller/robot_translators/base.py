"""
Base class for robot protocol translators.
"""

import logging
from abc import ABC, abstractmethod
from typing import Optional


class RobotTranslator(ABC):
    """
    Base class for robot protocol translators.

    Translators convert VR client robot control messages into robot-specific protocols.
    """

    def __init__(self):
        """Initialize the robot translator."""
        self.logger = logging.getLogger(self.__class__.__name__)

    @abstractmethod
    def translate(self, vr_packet: bytes) -> Optional[bytes]:
        """
        Translate VR client robot control packet to robot-specific protocol.

        Args:
            vr_packet: Raw packet from VR client (21 bytes)
                      Format: [0x02] [linear_x (float)] [linear_y (float)] [angular (float)] [timestamp (uint64)]

        Returns:
            Translated packet for robot, or None if translation failed
        """
        pass

    @abstractmethod
    def get_name(self) -> str:
        """
        Get translator name.

        Returns:
            Human-readable translator name
        """
        pass
