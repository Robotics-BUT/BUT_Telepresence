"""
Abstract base class for servo translators.

Different robots use different servo drivers and protocols. This base class
defines the interface that all servo translators must implement, and drive
supervision: a translator that can observe its drive implements health() (and
the fault reset), and the base class watches it, logs changes and reports them.
The relay and the headset see only ServoHealth, never a drive's own codes.
"""

import logging
import socket
import threading
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Callable, Tuple, Optional


@dataclass(frozen=True)
class ServoHealth:
    """The servo drive's state as a translator can observe it."""
    answering: bool          # the drive replies to the translator
    fault: bool = False      # the drive has latched a fault and ignores motion commands until reset
    description: str = ""    # what the fault is, in words for the operator (empty without a fault)


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
        self._supervisor: Optional[threading.Thread] = None
        self._supervisor_stop = threading.Event()

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

    # Drive supervision. A translator that can observe its drive overrides health(), and the reset if it has one;
    # start_supervision() then watches it.

    SUPERVISION_PERIOD_S = 0.2   # how often health() is asked
    SUPERVISION_REPORT_S = 1.0   # an unchanged state is reported this often

    def health(self) -> Optional[ServoHealth]:
        """The drive's current state, or None if this translator cannot observe it."""
        return None

    def start_supervision(self, report: Callable[[ServoHealth], None]):
        """Watch the drive's health on a thread of its own: log a fault appearing or clearing and the drive going
        silent, and call report(health) at once on a change and at least every SUPERVISION_REPORT_S. Does nothing for
        a translator that cannot observe its drive."""
        if self._supervisor is not None or type(self).health is ServoTranslator.health:
            return
        self._supervisor_stop.clear()
        self._supervisor = threading.Thread(target=self._supervise, args=(report,), daemon=True, name="servo-health")
        self._supervisor.start()

    def stop_supervision(self):
        self._supervisor_stop.set()
        if self._supervisor is not None:
            self._supervisor.join(timeout=2.0)
            self._supervisor = None

    def _supervise(self, report: Callable[[ServoHealth], None]):
        log = logging.getLogger(type(self).__module__)
        last: Optional[ServoHealth] = None
        reported_at = 0.0
        while not self._supervisor_stop.wait(self.SUPERVISION_PERIOD_S):
            try:
                health = self.health()
            except Exception as e:
                log.debug(f"servo health error: {e}")
                continue
            if health is None:
                continue
            if health != last:
                if not health.answering:
                    log.warning("Servo drive not answering")
                elif health.fault:
                    hint = " (reset: headset Camera tab, or python3 -m robot_controller.servo_reset)" \
                        if self.supports_fault_reset() else ""
                    log.warning(f"Servo fault: {health.description}; the drive ignores commands until reset{hint}")
                elif last is not None:
                    log.info("Servo drive healthy" + (" again, fault cleared" if last.fault else " again"))
            now = time.monotonic()
            if health != last or now - reported_at >= self.SUPERVISION_REPORT_S:
                try:
                    report(health)
                except Exception as e:
                    log.debug(f"servo health report error: {e}")
                reported_at = now
            last = health

    def supports_fault_reset(self) -> bool:
        """Whether reset_faults() can clear a latched fault on this drive."""
        return False

    def reset_faults(self) -> bool:
        """Clear the drive's latched faults and enable it again; the next command moves it. Returns whether a reset
        was sent."""
        return False
