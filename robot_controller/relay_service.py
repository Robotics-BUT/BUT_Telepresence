"""
Main UDP relay service for robot control.

This service receives commands via UDP, routes them to the appropriate
destination (servo driver or robot), and returns responses.
"""

import logging
import signal
import socket
import struct
import sys
import time
from threading import Thread, Lock
from typing import Optional, Tuple, List

from .config import load_configuration, RelayConfig
from .exceptions import RelayServiceError
from .protocol import MessageDetector, MessageType
from .servo_translators import ServoTranslator, TGDrivesTranslator

try:
    from influxdb_client_3 import InfluxDBClient3, Point
    INFLUXDB_AVAILABLE = True
except ImportError:
    INFLUXDB_AVAILABLE = False


class UDPRelayService:
    """
    Main UDP relay service.

    Receives UDP messages on ingest port, routes them based on message type:
    - Head pose messages (0x01 prefix) -> servo translator -> servo driver
    - Robot control messages (0x02 prefix) -> robot controller
    """

    def __init__(self, config: RelayConfig):
        """
        Initialize the UDP relay service.

        Args:
            config: Service configuration
        """
        self.config = config
        self.logger = logging.getLogger(__name__)

        # Sockets
        self.ingest_socket: Optional[socket.socket] = None
        self.robot_socket: Optional[socket.socket] = None

        # Protocol components
        self.message_detector = MessageDetector()

        # Servo translator (initialized based on config)
        self.servo_translator: Optional[ServoTranslator] = None

        # State
        self.running = False
        self.consecutive_errors = 0

        # Telemetry - InfluxDB with batch buffering
        self.influx_client: Optional[InfluxDBClient3] = None
        self.influx_buffer: List[Point] = []
        self.influx_buffer_lock = Lock()
        self.influx_batch_thread: Optional[Thread] = None

    def _init_influxdb(self):
        """Initialize InfluxDB client if telemetry is enabled."""
        if not self.config.telemetry_enabled:
            self.logger.info("Telemetry disabled")
            return

        if not INFLUXDB_AVAILABLE:
            self.logger.warning("Telemetry enabled but influxdb3-python package not installed. "
                              "Install with: pip install influxdb3-python")
            return

        try:
            self.influx_client = InfluxDBClient3(
                host=self.config.influxdb_host,
                database=self.config.influxdb_database,
                token=self.config.influxdb_token
            )

            # Start batch writer thread (flushes every 2 seconds)
            self.influx_batch_thread = Thread(target=self._influx_batch_writer, daemon=True)
            self.influx_batch_thread.start()

            self.logger.info(f"InfluxDB telemetry enabled (batch mode, 2s interval): {self.config.influxdb_host}/{self.config.influxdb_database}")
        except Exception as e:
            self.logger.error(f"Failed to initialize InfluxDB client: {e}")
            self.influx_client = None

    def _influx_batch_writer(self):
        """Background thread that batches and writes points every 2 seconds."""
        self.logger.info("InfluxDB batch writer thread started")

        while self.running:
            try:
                # Sleep for 2 seconds
                time.sleep(2.0)

                # Get all buffered points
                with self.influx_buffer_lock:
                    if not self.influx_buffer:
                        continue  # Nothing to write

                    points_to_write = self.influx_buffer[:]
                    self.influx_buffer.clear()

                # Batch write all points
                if points_to_write:
                    try:
                        self.influx_client.write(record=points_to_write)
                        self.logger.debug(f"Batch wrote {len(points_to_write)} points to InfluxDB")
                    except Exception as e:
                        self.logger.warning(f"Failed to batch write to InfluxDB: {e}")

            except Exception as e:
                self.logger.error(f"Error in batch writer thread: {e}")

        # Final flush on shutdown
        with self.influx_buffer_lock:
            if self.influx_buffer:
                try:
                    self.influx_client.write(record=self.influx_buffer)
                    self.logger.info(f"Final flush: wrote {len(self.influx_buffer)} points")
                except Exception:
                    pass

        self.logger.info("InfluxDB batch writer thread stopped")

    def _create_servo_translator(self) -> ServoTranslator:
        """
        Create servo translator based on configuration.

        Returns:
            Servo translator instance

        Raises:
            RelayServiceError: If translator type is unknown
        """
        translator_type = self.config.servo_translator.lower()

        if translator_type == 'tg_drives':
            return TGDrivesTranslator(
                self.config.servo_ip,
                self.config.servo_port,
                self.config.servo_response_timeout,
                azimuth_min=self.config.tg_azimuth_min,
                azimuth_max=self.config.tg_azimuth_max,
                elevation_min=self.config.tg_elevation_min,
                elevation_max=self.config.tg_elevation_max,
                speed_max=self.config.tg_speed_max,
                speed_multiplier=self.config.tg_speed_multiplier,
                filter_alpha=self.config.tg_filter_alpha,
                swap_axes=self.config.tg_swap_axes,
                invert_azimuth=self.config.tg_invert_azimuth,
                invert_elevation=self.config.tg_invert_elevation
            )
        else:
            raise RelayServiceError(f"Unknown servo translator type: {translator_type}")

    def _initialize_sockets(self) -> bool:
        """
        Initialize UDP sockets.

        Returns:
            True if all sockets initialized successfully, False otherwise
        """
        try:
            # Create ingest socket
            self.ingest_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.ingest_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.ingest_socket.bind((self.config.ingest_host, self.config.ingest_port))
            self.logger.info(f"Ingest socket bound to {self.config.ingest_host}:{self.config.ingest_port}")

            # Create robot socket
            self.robot_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.robot_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.robot_socket.bind(("0.0.0.0", 0))  # Bind to any available port
            self.robot_socket.settimeout(self.config.robot_response_timeout)
            self.logger.info("Robot socket initialized")

            # Initialize servo translator
            self.servo_translator = self._create_servo_translator()
            if not self.servo_translator.initialize():
                self.logger.error("Failed to initialize servo translator")
                return False

            self.logger.info("All sockets initialized successfully")
            return True

        except Exception as e:
            self.logger.error(f"Failed to initialize sockets: {e}")
            self._close_sockets()
            return False

    def _close_sockets(self):
        """Close all sockets."""
        if self.ingest_socket:
            try:
                self.ingest_socket.close()
            except Exception as e:
                self.logger.warning(f"Error closing ingest socket: {e}")
            self.ingest_socket = None

        if self.robot_socket:
            try:
                self.robot_socket.close()
            except Exception as e:
                self.logger.warning(f"Error closing robot socket: {e}")
            self.robot_socket = None

        if self.servo_translator:
            try:
                self.servo_translator.close()
            except Exception as e:
                self.logger.warning(f"Error closing servo translator: {e}")
            self.servo_translator = None

        if self.influx_client:
            try:
                # Wait for batch thread to finish (it will flush remaining data)
                if self.influx_batch_thread and self.influx_batch_thread.is_alive():
                    self.logger.info("Waiting for InfluxDB batch writer to finish...")
                    self.influx_batch_thread.join(timeout=5.0)

                self.influx_client.close()
                self.logger.info("InfluxDB client closed")
            except Exception as e:
                self.logger.warning(f"Error closing InfluxDB client: {e}")
            self.influx_client = None

    def _route_message(self, data: bytes, client_addr: Tuple[str, int]):
        """
        Route message to appropriate destination.

        Args:
            data: Raw message data
            client_addr: Client address (IP, port)
        """
        # Detect message type
        message_type = self.message_detector.detect_message_type(data)

        if message_type == MessageType.HEAD_POSE:
            self._forward_to_servo(data, client_addr)

        elif message_type == MessageType.ROBOT_CONTROL:
            self._forward_to_robot(data, client_addr)

        elif message_type == MessageType.DEBUG_INFO:
            self._handle_debug_info(data, client_addr)

        else:
            self.logger.warning(f"Unknown message type from {client_addr[0]}:{client_addr[1]}, dropping")

    def _forward_to_servo(self, data: bytes, client_addr: Tuple[str, int]):
        """
        Forward message to servo driver via translator.

        Args:
            data: Servo command data
            client_addr: Client address to send response to
        """
        if not self.servo_translator:
            self.logger.error("Servo translator not initialized")
            return

        try:
            # Translator handles protocol conversion, sending, and receiving
            response = self.servo_translator.translate_and_forward(data, client_addr)

            # Send response back to client if we got one
            if response and self.ingest_socket:
                self.ingest_socket.sendto(response, client_addr)
                self.logger.debug(f"Response sent back to {client_addr[0]}:{client_addr[1]}")

            # Reset error counter on success
            self.consecutive_errors = 0

        except Exception as e:
            self.consecutive_errors += 1
            self.logger.error(f"Error forwarding to servo: {e}")

    def _forward_to_robot(self, data: bytes, client_addr: Tuple[str, int]):
        """
        Forward message to robot controller.

        Args:
            data: Robot command data
            client_addr: Client address (not used for robot commands)
        """
        if not self.robot_socket:
            self.logger.error("Robot socket not initialized")
            return

        try:
            self.robot_socket.sendto(data, (self.config.robot_ip, self.config.robot_port))
            self.logger.debug(f"Forwarded robot command to {self.config.robot_ip}:{self.config.robot_port}")

            # Robot commands typically don't expect responses
            # If they do in the future, add response handling here

            # Reset error counter on success
            self.consecutive_errors = 0

        except Exception as e:
            self.consecutive_errors += 1
            self.logger.error(f"Error forwarding to robot: {e}")

    def _handle_debug_info(self, data: bytes, client_addr: Tuple[str, int]):
        """
        Handle debug info message.

        Args:
            data: Debug info data
            client_addr: Client address

        Message format (78 bytes):
            [0x03] [timestamp (uint64)] [frame_id (uint64)] [fps (float)]
            [vidConv_us (uint64)] [enc_us (uint64)] [rtpPay_us (uint64)] [udpStream_us (uint64)]
            [rtpDepay_us (uint64)] [dec_us (uint64)] [presentation_us (uint64)]
            [ntp_offset_us (int64)] [ntp_synced (uint8)] [time_since_ntp_sync_us (uint64)]
        """
        try:
            # Validate packet length (currently 98 bytes)
            # Note: FPS appears to be serialized as 8 bytes (double) instead of 4 bytes (float)
            expected_length = 98
            if len(data) != expected_length:
                self.logger.warning(f"Invalid debug info packet length: {len(data)} bytes, expected {expected_length}")
                return

            # Parse the message
            offset = 1  # Skip message type byte

            timestamp = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            frame_id = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            fps = struct.unpack('<d', data[offset:offset+8])[0]
            offset += 8

            vidConv_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            enc_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            rtpPay_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            udpStream_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            rtpDepay_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            dec_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            presentation_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            ntp_offset_us = struct.unpack('<q', data[offset:offset+8])[0]
            offset += 8

            ntp_synced = struct.unpack('<B', data[offset:offset+1])[0]
            offset += 1

            time_since_ntp_sync_us = struct.unpack('<Q', data[offset:offset+8])[0]
            offset += 8

            # Log the debug information
            self.logger.debug(
                f"DEBUG INFO from {client_addr[0]}:{client_addr[1]} - "
                f"frame_id={frame_id}, fps={fps:.1f}, ts={timestamp}, "
                f"pipeline_us=[vidConv={vidConv_us}, enc={enc_us}, rtpPay={rtpPay_us}, "
                f"udpStream={udpStream_us}, rtpDepay={rtpDepay_us}, dec={dec_us}, pres={presentation_us}], "
                f"ntp=[offset_us={ntp_offset_us}, synced={ntp_synced}, time_since_sync_us={time_since_ntp_sync_us}]"
            )

            # Write to InfluxDB if enabled
            if self.influx_client:
                try:
                    # Calculate total pipeline latency
                    total_latency_us = vidConv_us + enc_us + rtpPay_us + udpStream_us + rtpDepay_us + dec_us + presentation_us

                    # Create Point using influxdb3-python API
                    # Use current time instead of packet timestamp (which is relative, not Unix epoch)
                    timestamp_ns = time.time_ns()

                    point = (
                        Point("pipeline_metrics")
                        .tag("source", client_addr[0])
                        .field("frame_id", int(frame_id))
                        .field("fps", float(fps))
                        .field("vidConv_us", int(vidConv_us))
                        .field("enc_us", int(enc_us))
                        .field("rtpPay_us", int(rtpPay_us))
                        .field("udpStream_us", int(udpStream_us))
                        .field("rtpDepay_us", int(rtpDepay_us))
                        .field("dec_us", int(dec_us))
                        .field("presentation_us", int(presentation_us))
                        .field("total_latency_us", int(total_latency_us))
                        .field("ntp_offset_us", int(ntp_offset_us))
                        .field("ntp_synced", int(ntp_synced))
                        .field("time_since_ntp_sync_us", int(time_since_ntp_sync_us))
                        .time(timestamp_ns)
                    )

                    # Buffer point for batch write (non-blocking)
                    with self.influx_buffer_lock:
                        self.influx_buffer.append(point)
                    self.logger.debug(f"Buffered metrics for InfluxDB: frame_id={frame_id}, fps={fps:.1f}, buffer_size={len(self.influx_buffer)}")
                except Exception as e:
                    self.logger.warning(f"Failed to buffer InfluxDB point: {e}")

            # Reset error counter on success
            self.consecutive_errors = 0

        except struct.error as e:
            self.consecutive_errors += 1
            self.logger.error(f"Error parsing debug info: {e}")
        except Exception as e:
            self.consecutive_errors += 1
            self.logger.error(f"Error handling debug info: {e}")

    def _listen_loop(self):
        """
        Main message receiving loop.

        Receives messages from ingest socket and routes them.
        """
        self.logger.info("Entering main listen loop")

        while self.running:
            try:
                # Receive message
                data, client_addr = self.ingest_socket.recvfrom(self.config.socket_buffer_size)

                self.logger.debug(f"Received {len(data)} bytes from {client_addr[0]}:{client_addr[1]}")

                # Route message
                self._route_message(data, client_addr)

                # Check for too many consecutive errors
                if self.consecutive_errors >= self.config.max_consecutive_errors:
                    self.logger.error(
                        f"Too many consecutive errors ({self.consecutive_errors}), stopping service"
                    )
                    self.running = False
                    break

            except KeyboardInterrupt:
                self.logger.info("Keyboard interrupt received")
                self.running = False
                break

            except Exception as e:
                self.consecutive_errors += 1
                self.logger.error(f"Error in listen loop: {e}", exc_info=True)

                if self.consecutive_errors >= self.config.max_consecutive_errors:
                    self.logger.error(
                        f"Too many consecutive errors ({self.consecutive_errors}), stopping service"
                    )
                    self.running = False
                    break

        self.logger.info("Exited main listen loop")

    def start(self):
        """
        Start the relay service.

        This method blocks until the service is stopped.
        """
        self.logger.info("Starting UDP Relay Service")
        self.logger.info(f"Configuration: {self.config}")

        # Initialize sockets
        if not self._initialize_sockets():
            self.logger.error("Failed to initialize sockets, exiting")
            raise RelayServiceError("Socket initialization failed")

        # Set running flag
        self.running = True

        # Init telemetry
        self._init_influxdb()

        # Enter main loop
        try:
            self._listen_loop()
        finally:
            self.stop()

    def stop(self):
        """Stop the relay service and clean up resources."""
        self.logger.info("Stopping relay service")
        self.running = False

        # Close all sockets
        self._close_sockets()

        self.logger.info("Relay service stopped")


# Global service instance for signal handling
_service_instance: Optional[UDPRelayService] = None


def signal_handler(signum, frame):
    """Handle termination signals for graceful shutdown."""
    logger = logging.getLogger(__name__)
    logger.info(f"Received signal {signum}, initiating shutdown")

    global _service_instance
    if _service_instance:
        _service_instance.stop()


def setup_logging(log_level: str, log_file: Optional[str] = None):
    """
    Setup logging configuration.

    Args:
        log_level: Logging level (DEBUG, INFO, WARNING, ERROR)
        log_file: Optional log file path
    """
    level = getattr(logging, log_level.upper())

    # Create formatter
    formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')

    # Setup root logger
    logger = logging.getLogger()
    logger.setLevel(level)

    # Remove any existing handlers
    logger.handlers = []

    # Console handler
    console_handler = logging.StreamHandler(sys.stdout)
    console_handler.setLevel(level)
    console_handler.setFormatter(formatter)
    logger.addHandler(console_handler)

    # File handler if specified
    if log_file:
        file_handler = logging.FileHandler(log_file)
        file_handler.setLevel(level)
        file_handler.setFormatter(formatter)
        logger.addHandler(file_handler)


def main():
    """Main entry point for the relay service."""
    try:
        # Load configuration
        config = load_configuration()

        # Setup logging
        setup_logging(config.log_level, config.log_file)

        logger = logging.getLogger(__name__)
        logger.info("BUT Telepresence Robot Control Relay Service")

        # Setup signal handlers
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)

        # Create and start service
        global _service_instance
        _service_instance = UDPRelayService(config)
        _service_instance.start()

        return 0

    except KeyboardInterrupt:
        logging.info("Keyboard interrupt received")
        return 0

    except Exception as e:
        logging.error(f"Fatal error: {e}", exc_info=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
