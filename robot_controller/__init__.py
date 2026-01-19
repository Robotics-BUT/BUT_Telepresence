"""
Robot Controller - UDP Relay Service for Robot Control

This package provides a UDP relay service that routes commands to different
robot subsystems:
- Head pose commands (0x01 prefix) -> Servo driver via translator
- Robot movement commands (0x02 prefix) -> Robot controller
- Debug info (0x03 prefix) -> Logging

The servo translation layer is abstracted to support different robot types
with different proprietary servo drivers.
"""

__version__ = "1.0.0"
__author__ = "Stanislav Svědiroh / Brno University of Technology"

from .config import RelayConfig, load_configuration
from .exceptions import (
    RelayServiceError,
    ConfigurationError,
    SocketError,
    ProtocolError,
    TimeoutError
)
from .protocol import MessageDetector, MessageType
from .relay_service import UDPRelayService
from .servo_translators import ServoTranslator, TGDrivesTranslator

__all__ = [
    '__version__',
    'RelayConfig',
    'load_configuration',
    'RelayServiceError',
    'ConfigurationError',
    'SocketError',
    'ProtocolError',
    'TimeoutError',
    'MessageDetector',
    'MessageType',
    'UDPRelayService',
    'ServoTranslator',
    'TGDrivesTranslator',
]
