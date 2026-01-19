"""
Custom exceptions for the UDP relay service.
"""


class RelayServiceError(Exception):
    """Base exception for all relay service errors."""
    pass


class ConfigurationError(RelayServiceError):
    """Raised when configuration is invalid or incomplete."""
    pass


class SocketError(RelayServiceError):
    """Raised when a socket operation fails."""
    pass


class ProtocolError(RelayServiceError):
    """Raised when a protocol message is invalid or malformed."""
    pass


class TimeoutError(RelayServiceError):
    """Raised when a response timeout is exceeded."""
    pass
