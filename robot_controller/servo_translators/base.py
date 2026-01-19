"""
Abstract base class for servo translators.

Different robots use different servo drivers and protocols. This base class
defines the interface that all servo translators must implement.
"""

import socket
from abc import ABC, abstractmethod
from typing import Tuple, Optional


class ServoTranslator(ABC):
    """
    Abstract base class for servo protocol translation.

    Each robot type should implement this interface to translate
    incoming servo commands to the format expected by their
    specific servo driver.
    """

    def __init__(self, servo_ip: str, servo_port: int, timeout: float):
        """
        Initialize the servo translator.

        Args:
            servo_ip: IP address of the servo driver
            servo_port: Port of the servo driver
            timeout: Response timeout in seconds
        """
        self.servo_ip = servo_ip
        self.servo_port = servo_port
        self.timeout = timeout
        self.socket: Optional[socket.socket] = None

    @abstractmethod
    def initialize(self) -> bool:
        """
        Initialize the servo translator (create socket, etc.).

        Returns:
            True if initialization succeeded, False otherwise
        """
        pass

    @abstractmethod
    def translate_and_forward(self, data: bytes, client_addr: Tuple[str, int]) -> Optional[bytes]:
        """
        Translate incoming command and forward to servo driver.

        This method should:
        1. Parse the incoming command
        2. Translate it to the servo driver's format
        3. Send to the servo driver
        4. Wait for response (if expected)
        5. Return the response

        Args:
            data: Raw command data from client
            client_addr: Address of the client (IP, port)

        Returns:
            Response bytes to send back to client, or None if no response
        """
        pass

    @abstractmethod
    def close(self):
        """
        Clean up resources (close socket, etc.).
        """
        pass
