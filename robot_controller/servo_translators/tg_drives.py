"""
TG Drives servo translator implementation.

Translates simple azimuth/elevation messages to TG Drives GT protocol.

Incoming format (41 bytes):
    Byte 0:      0x01 (message type)
    Bytes 1-4:   azimuth (float32, little-endian) in radians, the forecast pose commanded to the camera head
    Bytes 5-8:   elevation (float32, little-endian) in radians, the same
    Bytes 9-12:  speed (float32, little-endian) in motor units
    Bytes 13-20: timestamp (uint64, little-endian), microseconds, NTP-corrected to the robot clock (send time)
    Bytes 21-24: prediction_ms (uint32, little-endian), consumed by the relay
    Bytes 25-28: measured azimuth (float32) in radians: the head pose at the present instant, not forecast
    Bytes 29-32: measured elevation (float32) in radians
    Bytes 33-40: measured pose sample time (uint64), microseconds, NTP-corrected to the robot clock

Position read-back (TGZ48 register protocol, as in asgard-hardwaresupport TGZ48Packet): a separate thread and
socket send one SequenceRegisterRead (0x03) per register pair and parse the reply
'GT' + [op, reg(BE16), status, count, count x value(LE32)]... The 64-bit position is revolution (high 32 bits,
signed) and angle (low 32 bits), as on the write side. Motor 1 = register group 0x19 (ELEVATION), motor 2 = 0x1A
(AZIMUTH): Monitor1/2_aAngle+aRevol (0x1310/0x1410) is the measured position, PG1/2_APosAngle+APosRevol
(0x1902/0x1A02) the position generator's current setpoint. The same poll reads each axis' drive status and latched
error (Monitor1/2_aDriveStatus+aDriveError, 0x1325/0x1425).

Drive supervision (ServoTranslator.health / reset_faults): a drive that latches an error (over-current, following
error, e-stop, ...) ignores motion commands until 0x08 is written to its command register (Control1/2_Command,
0x1100/0x1200, the register the enable writes 1 to). health() reports the latched errors in words, from the
read-back's drive state or, without a recent one, from a read of its own; reset_faults() clears both axes and enables
them again. The error codes stay in this translator.

The head pose is smoothed with a One Euro filter (one_euro.py) in radians, with time steps taken from the
headset timestamps, before conversion to motor units. The conversion is 1:1: both drives turn the camera directly and a
position carries 2^32 units per revolution in its angle word, so a head angle commands the same camera angle about the
centre of the axis' travel limits. The operator's sensitivity is applied in the headset, to the angles it sends.

Outgoing format (GT protocol):
    Bytes 0-1:  0x47, 0x54 ("GT" magic bytes)
    Then multiple operations for azimuth/elevation angle, speed, and enable
"""

import logging
import socket
import struct
import threading
import time
from typing import Callable, Tuple, Optional, Dict
import math

from .base import ServoHealth, ServoTranslator
from .one_euro import OneEuroFilter


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
    PG_ACC = 0x00      # profile generator: acceleration, inc/s^2
    PG_DEC = 0x01      # profile generator: deceleration, inc/s^2
    PG_TYPE = 0x0B     # profile generator: 0 harmonic non-sym., 1 harmonic sym., 2 full harmonic, 3 trapezoidal

    PACKET_LENGTH = 41

    # Read-back registers (TGZ48): operation, (register, count) pairs
    SEQUENCE_REGISTER_READ = 0x03
    STATUS_OK = 0x00
    READBACK_REGISTERS = {
        "act_el": 0x1310,   # Monitor1_aAngle, aRevol: measured position, motor 1 (group 0x19, ELEVATION)
        "act_az": 0x1410,   # Monitor2_aAngle, aRevol: measured position, motor 2 (group 0x1A, AZIMUTH)
        "pg_el": 0x1902,    # PG1_APosAngle, APosRevol: position generator setpoint, motor 1
        "pg_az": 0x1A02,    # PG2_APosAngle, APosRevol: position generator setpoint, motor 2
    }
    DRIVE_STATE_REGISTERS = {
        "el": 0x1325,       # Monitor1_aDriveStatus, aDriveError
        "az": 0x1425,       # Monitor2_aDriveStatus, aDriveError
    }

    # Command register value that clears an axis' latched errors (TGZ48 Commands::ResetErrors)
    RESET_ERRORS = 0x08
    # aDriveError bits (TGZ48Common::DriveError in asgard-hardwaresupport)
    DRIVE_ERRORS = {
        0x00000004: "Internal", 0x00000008: "OverVoltage", 0x00000010: "LowVoltage", 0x00000040: "StaticBrake",
        0x00000200: "MotorOverTemperature", 0x00000800: "DriveOverTemperature", 0x00001000: "Feedback",
        0x00008000: "Position", 0x00010000: "Trajectory", 0x00020000: "FieldBus", 0x00100000: "EmergencyStop",
        0x00200000: "OverCurrent", 0x08000000: "Parameter",
    }

    # Head-pose filter time-step bounds (seconds)
    FILTER_RESET_S = 0.5     # longer gap between samples: restart the filters at the new pose
    FILTER_MIN_DT_S = 0.001  # guard against duplicate or near-duplicate timestamps
    UNITS_PER_RAD = 2 ** 32 / (2 * math.pi)   # position units per radian of camera rotation (direct drive)

    def __init__(self, servo_ip: str, servo_port: int, timeout: float,
                 azimuth_min: int = -180000, azimuth_max: int = 180000,
                 elevation_min: int = -90000, elevation_max: int = 90000,
                 speed_max: int = 1000000,
                 pg_acc: Optional[int] = None, pg_dec: Optional[int] = None, pg_type: Optional[int] = None,
                 filter_min_cutoff: float = 1.0, filter_beta: float = 20.0,
                 filter_d_cutoff: float = 5.0, swap_axes: bool = False,
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
            speed_max: Largest profile-generator speed (PG PosSpeed, inc/s) written to the drive; the speed in each
                head-pose packet is clamped to it. With BitsPerRevol = 20, 1,000,000 inc/s = 0.95 motor rev/s.
            pg_acc, pg_dec: profile-generator acceleration / deceleration (inc/s^2) written to both axes at start-up;
                None = leave the drive's stored values
            pg_type: profile type written at start-up (0 harmonic non-symmetric, 1 harmonic symmetric,
                2 full harmonic, 3 trapezoidal); None = leave the drive's stored value
            filter_min_cutoff: One Euro cutoff at rest, Hz (lower = smoother when the head is still)
            filter_beta: One Euro cutoff increase per rad/s of head speed (higher = less lag when turning)
            filter_d_cutoff: One Euro cutoff of the speed estimate, Hz
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
        self.pg_params = {item: value for item, value in ((self.PG_ACC, pg_acc), (self.PG_DEC, pg_dec),
                                                         (self.PG_TYPE, pg_type)) if value is not None}
        self.swap_axes = swap_axes
        self.invert_azimuth = invert_azimuth
        self.invert_elevation = invert_elevation

        # Filtering: One Euro per axis, in radians, on the headset's sample clock
        self.azimuth_filter = OneEuroFilter(filter_min_cutoff, filter_beta, filter_d_cutoff)
        self.elevation_filter = OneEuroFilter(filter_min_cutoff, filter_beta, filter_d_cutoff)

        # Out-of-order packet guard
        self.last_timestamp = 0

        # Per-packet record of what was commanded, read by the relay for telemetry
        self.last_command: Optional[Dict] = None

        # Position read-back thread
        self._poll_thread: Optional[threading.Thread] = None
        self._poll_stop = threading.Event()
        # The drive state from the last answered read-back: (monotonic time, {axis: (status, error)})
        self._drive_state: Optional[Tuple[float, Dict[str, Tuple[int, int]]]] = None

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

            # Set mode to position control on both axes, and the profile generator's ramps if configured
            self._set_mode()
            self._set_profile_generator()

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

    def _set_profile_generator(self):
        """Write the configured profile-generator parameters (Acc, Dec, Type) to both axes (working values)."""
        if not self.pg_params:
            return
        msg = bytearray([self.IDENTIFIER_1, self.IDENTIFIER_2])
        for group in (self.AZIMUTH, self.ELEVATION):
            for item, value in self.pg_params.items():
                msg.extend([self.WRITE, group, item])
                msg.extend(struct.pack('<I', int(value) & 0xFFFFFFFF))
        try:
            self.socket.sendto(bytes(msg), (self.servo_ip, self.servo_port))
            self.logger.info(f"Profile generator set on both axes: {self.pg_params}")
        except Exception as e:
            self.logger.warning(f"Failed to set the profile generator: {e}")

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

        # Validate packet length (see the module docstring). prediction_ms is consumed by the relay, not here.
        if len(data) != self.PACKET_LENGTH:
            self.logger.warning(f"Invalid packet length: {len(data)} bytes, expected {self.PACKET_LENGTH}")
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
            prediction_ms = struct.unpack('<I', data[21:25])[0]
            meas_az_rad, meas_el_rad = struct.unpack('<ff', data[25:33])
            meas_time_us = struct.unpack('<Q', data[33:41])[0]
        except struct.error as e:
            self.logger.error(f"Failed to parse message: {e}")
            return None

        # Guard against out-of-order packets
        if timestamp <= self.last_timestamp:
            self.logger.warning(f"Out-of-order packet dropped: ts={timestamp}, last_ts={self.last_timestamp}")
            return None

        # Time step on the headset's sample clock (us). A gap longer than FILTER_RESET_S (stream paused, app
        # relaunched) restarts the filters at the new pose instead of easing across the gap.
        dt_s = (timestamp - self.last_timestamp) / 1e6 if self.last_timestamp else 0.0
        self.last_timestamp = timestamp
        if not 0.0 < dt_s <= self.FILTER_RESET_S:
            self.azimuth_filter.reset()
            self.elevation_filter.reset()
            dt_s = self.FILTER_RESET_S
        dt_s = max(dt_s, self.FILTER_MIN_DT_S)

        self.logger.debug(f"Received: azimuth={azimuth_rad:.3f} rad, elevation={elevation_rad:.3f} rad, speed={speed_motor}, ts={timestamp}")

        # Into the servo frame (swap, invert), smooth on the sample clock, convert to motor units, clamp
        cmd_az_rad, cmd_el_rad = self._to_servo_frame(azimuth_rad, elevation_rad)
        filt_az_rad = self.azimuth_filter(cmd_az_rad, dt_s)
        filt_el_rad = self.elevation_filter(cmd_el_rad, dt_s)
        azimuth_motor, elevation_motor = self._convert_to_motor_units(filt_az_rad, filt_el_rad)

        # The measured head pose through the same mapping, unfiltered and unclamped, so that head, command and
        # servo position can be compared in one unit
        meas_az_srv, meas_el_srv = self._to_servo_frame(meas_az_rad, meas_el_rad)
        meas_az_motor, meas_el_motor = self._rad_to_motor(meas_az_srv, meas_el_srv)
        fc_az_motor, fc_el_motor = self._rad_to_motor(cmd_az_rad, cmd_el_rad)

        self.last_command = {
            "send_time_us": timestamp, "meas_time_us": meas_time_us, "prediction_ms": prediction_ms,
            "meas_az_rad": meas_az_srv, "meas_el_rad": meas_el_srv,
            "fc_az_rad": cmd_az_rad, "fc_el_rad": cmd_el_rad,
            "filt_az_rad": filt_az_rad, "filt_el_rad": filt_el_rad,
            "meas_az_motor": meas_az_motor, "meas_el_motor": meas_el_motor,
            "fc_az_motor": fc_az_motor, "fc_el_motor": fc_el_motor,
            "cmd_az_motor": azimuth_motor, "cmd_el_motor": elevation_motor,
        }

        # Build GT protocol message
        speed = max(0, min(int(speed_motor), self.speed_max))
        gt_message = self._build_gt_message(azimuth_motor, elevation_motor, speed)

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

    def _to_servo_frame(self, azimuth_rad: float, elevation_rad: float) -> Tuple[float, float]:
        """Apply the configured axis swap and inversions (headset frame -> servo frame)."""
        if self.swap_axes:
            azimuth_rad, elevation_rad = elevation_rad, azimuth_rad
        if self.invert_azimuth:
            azimuth_rad = -azimuth_rad
        if self.invert_elevation:
            elevation_rad = -elevation_rad
        return azimuth_rad, elevation_rad

    def _convert_to_motor_units(self, azimuth_rad: float, elevation_rad: float) -> Tuple[int, int]:
        """Convert to motor units and clamp to the configured limits."""
        azimuth_motor, elevation_motor = self._rad_to_motor(azimuth_rad, elevation_rad)
        azimuth_motor = max(self.azimuth_min, min(self.azimuth_max, azimuth_motor))
        elevation_motor = max(self.elevation_min, min(self.elevation_max, elevation_motor))
        return azimuth_motor, elevation_motor

    def _rad_to_motor(self, azimuth_rad: float, elevation_rad: float) -> Tuple[int, int]:
        """
        Convert azimuth/elevation from radians to motor units, without clamping.

        Args:
            azimuth_rad: Azimuth in radians
            elevation_rad: Elevation in radians

        Returns:
            Tuple of (azimuth_motor, elevation_motor) in motor units
        """
        # Head angle 0 points the camera at the centre of each axis' travel limits; one radian of head turn is one
        # radian of camera turn
        azimuth_center = self.azimuth_max - (self.azimuth_max - self.azimuth_min) // 2
        elevation_center = self.elevation_max - (self.elevation_max - self.elevation_min) // 2
        azimuth_motor = int(azimuth_center + azimuth_rad * self.UNITS_PER_RAD)
        elevation_motor = int(elevation_center - elevation_rad * self.UNITS_PER_RAD)

        return azimuth_motor, elevation_motor

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
        # Position is a 64-bit value carried as angle (low 32 bits) + revolution
        # (high 32 bits) for multi-turn servos. Packing the whole value into the
        # int32 angle field capped the range at +/-2147483647 and dropped any
        # command beyond it (e.g. an elevation_min below -2.1e9). Splitting the
        # 64-bit value across the two fields lifts that cap. Within int32 this is
        # identical to the previous encoding (low word == old angle, high word ==
        # old revol of -1/0).
        az_angle_bytes = struct.pack('<I', azimuth & 0xFFFFFFFF)
        az_revol_bytes = struct.pack('<i', azimuth >> 32)
        el_angle_bytes = struct.pack('<I', elevation & 0xFFFFFFFF)
        el_revol_bytes = struct.pack('<i', elevation >> 32)
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

    # ------------------------------------------------------------------ error reset
    @classmethod
    def describe_error(cls, code: int) -> str:
        """Names of the bits set in an aDriveError value ('None' for 0)."""
        if not code:
            return "None"
        names = [name for bit, name in cls.DRIVE_ERRORS.items() if code & bit]
        rest = code & ~sum(cls.DRIVE_ERRORS)
        return "+".join(names + ([f"0x{rest:08x}"] if rest else []))

    STATE_FRESH_S = 0.5   # a read-back drive state older than this is read again for health()

    def health(self) -> Optional[ServoHealth]:
        """Whether the drive answers and which errors its axes have latched (ServoTranslator.health)."""
        state = self._drive_state
        if state is None or time.monotonic() - state[0] > self.STATE_FRESH_S:
            axes = self.read_drive_state(timeout=0.2)   # no recent read-back (off, or the drive stopped answering)
            if not axes:
                return ServoHealth(answering=False)
            state = (time.monotonic(), axes)
        errors = {axis: error for axis, (_, error) in state[1].items() if error}
        if not errors:
            return ServoHealth(answering=True)
        names = sorted({self.describe_error(e) for e in errors.values()})
        where = "both axes" if len(errors) == len(self.DRIVE_STATE_REGISTERS) else "one axis"
        return ServoHealth(answering=True, fault=True, description=f"{', '.join(names)} ({where})")

    def supports_fault_reset(self) -> bool:
        return True

    def reset_faults(self) -> bool:
        """Clear both axes' latched errors, then put them back in position mode and enable them; the next head pose
        moves them."""
        reset = bytearray([self.IDENTIFIER_1, self.IDENTIFIER_2])
        enable = bytearray([self.IDENTIFIER_1, self.IDENTIFIER_2])
        for group in (self.ENABLE_AZIMUTH, self.ENABLE_ELEVATION):
            reset.extend([self.WRITE, group, self.ENABLE] + list(struct.pack('<I', self.RESET_ERRORS)))
            enable.extend([self.WRITE, group, self.ENABLE, 0x01, 0x00, 0x00, 0x00])
        self.socket.sendto(bytes(reset), (self.servo_ip, self.servo_port))
        time.sleep(0.05)
        self._set_mode()
        self.socket.sendto(bytes(enable), (self.servo_ip, self.servo_port))
        self.logger.info("TG Drives: errors reset on both axes, axes enabled")
        return True

    def read_drive_state(self, timeout: float = 0.5) -> Optional[Dict[str, Tuple[int, int]]]:
        """One read of each axis' (drive status, drive error) on a socket of its own; None if the drive does not
        answer."""
        msg = bytearray([self.IDENTIFIER_1, self.IDENTIFIER_2])
        for reg in self.DRIVE_STATE_REGISTERS.values():
            msg.extend([self.SEQUENCE_REGISTER_READ, (reg >> 8) & 0xFF, reg & 0xFF, 0x02])
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(timeout)
            sock.sendto(bytes(msg), (self.servo_ip, self.servo_port))
            values = self.parse_readback(sock.recvfrom(1024)[0])
        except socket.timeout:
            return None
        finally:
            sock.close()
        return {axis: tuple(values[reg][:2]) for axis, reg in self.DRIVE_STATE_REGISTERS.items() if reg in values}

    # ------------------------------------------------------------------ position read-back
    def _build_readback_request(self) -> bytes:
        msg = bytearray([self.IDENTIFIER_1, self.IDENTIFIER_2])
        for reg in list(self.READBACK_REGISTERS.values()) + list(self.DRIVE_STATE_REGISTERS.values()):
            msg.extend([self.SEQUENCE_REGISTER_READ, (reg >> 8) & 0xFF, reg & 0xFF, 0x02])
        return bytes(msg)

    @staticmethod
    def position(values: Tuple[int, ...]) -> int:
        """The 64-bit position from an (angle, revolution) register pair."""
        return (struct.unpack('<i', struct.pack('<I', values[1]))[0] << 32) | values[0]

    @classmethod
    def parse_readback(cls, data: bytes) -> Dict[int, Tuple[int, ...]]:
        """
        Parse a TGZ48 reply into {first register: values (unsigned 32-bit)} for every sequence read with status OK.
        Layout per operation: op, register (BE16), status, then for a sequence read: count, count x LE32.
        Register writes in the same reply (4 bytes each) are skipped.
        """
        out: Dict[int, Tuple[int, ...]] = {}
        if len(data) < 6 or data[0] != cls.IDENTIFIER_1 or data[1] != cls.IDENTIFIER_2:
            return out
        i = 2
        while i + 4 <= len(data):
            op, reg, status = data[i], (data[i + 1] << 8) | data[i + 2], data[i + 3]
            if op == cls.SEQUENCE_REGISTER_READ:
                count = data[i + 4]
                values = struct.unpack_from('<' + 'I' * count, data, i + 5) if i + 5 + 4 * count <= len(data) else ()
                if status == cls.STATUS_OK and count >= 2 and values:
                    out[reg] = values
                i += 5 + 4 * count
            elif op == 0x01:                       # single register read
                i += 8 if status == cls.STATUS_OK else 4
            elif op in (cls.WRITE, cls.WRITE_CONTINUOUS):
                i += 4 if op == cls.WRITE else 5
            else:
                break
        return out

    def start_position_readback(self, rate_hz: float, callback: Callable[[Dict], None]):
        """
        Poll the drive's measured position, position-generator setpoint and drive state at rate_hz on a separate
        socket, and call callback(sample) with robot-clock send/receive times (ns), the 64-bit positions in motor
        units and each axis' drive status and error (status_el, error_el, status_az, error_az).
        A sample whose reply does not arrive within the timeout is reported with timeout=True.
        """
        if rate_hz <= 0 or self._poll_thread:
            return
        request = self._build_readback_request()
        period = 1.0 / rate_hz
        names = {reg: name for name, reg in self.READBACK_REGISTERS.items()}

        def run():
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.bind(("0.0.0.0", 0))
            sock.settimeout(min(0.05, period))
            next_t = time.monotonic()
            self.logger.info(f"TG Drives position read-back at {rate_hz:g} Hz")
            while not self._poll_stop.is_set():
                t_send = time.time_ns()
                try:
                    sock.sendto(request, (self.servo_ip, self.servo_port))
                    data, _ = sock.recvfrom(1024)
                    t_recv = time.time_ns()
                    values = self.parse_readback(data)
                    sample = {"t_send_ns": t_send, "t_recv_ns": t_recv, "timeout": False}
                    sample.update({names[r]: self.position(v) for r, v in values.items() if r in names})
                    axes = {axis: tuple(values[reg][:2]) for axis, reg in self.DRIVE_STATE_REGISTERS.items() if reg in values}
                    for axis, (status, error) in axes.items():
                        sample[f"status_{axis}"], sample[f"error_{axis}"] = status, error
                    if len(axes) == len(self.DRIVE_STATE_REGISTERS):
                        self._drive_state = (time.monotonic(), axes)
                    callback(sample)
                except socket.timeout:
                    callback({"t_send_ns": t_send, "t_recv_ns": time.time_ns(), "timeout": True})
                except Exception as e:
                    self.logger.debug(f"position read-back error: {e}")
                next_t += period
                delay = next_t - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                else:
                    next_t = time.monotonic()
            sock.close()

        self._poll_stop.clear()
        self._poll_thread = threading.Thread(target=run, daemon=True, name="tg-readback")
        self._poll_thread.start()

    def close(self):
        """Stop the read-back thread and close the UDP socket."""
        self._poll_stop.set()
        if self._poll_thread:
            self._poll_thread.join(timeout=1.0)
            self._poll_thread = None
        if self.socket:
            try:
                self.socket.close()
                self.logger.info("TG Drives translator closed")
            except Exception as e:
                self.logger.error(f"Error closing socket: {e}")
            finally:
                self.socket = None
