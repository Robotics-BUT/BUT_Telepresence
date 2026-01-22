"""
Configuration management for the robot controller relay service.

Configuration is loaded from config.yaml file.
"""

import os
from dataclasses import dataclass
from typing import Optional

import yaml

from .exceptions import ConfigurationError


@dataclass
class RelayConfig:
    """
    Configuration for the UDP relay service.
    """

    # Network settings - Ingest
    ingest_host: str = "0.0.0.0"
    ingest_port: int = 32115

    # Network settings - Servo driver
    servo_ip: str = "192.168.1.150"
    servo_port: int = 502
    servo_translator: str = "tg_drives"

    # TG Drives servo configuration
    tg_azimuth_min: int = -180000
    tg_azimuth_max: int = 180000
    tg_elevation_min: int = -90000
    tg_elevation_max: int = 90000
    tg_speed_max: int = 1000000
    tg_speed_multiplier: float = 0.0
    tg_filter_alpha: float = 0.15
    tg_swap_axes: bool = False
    tg_invert_azimuth: bool = False
    tg_invert_elevation: bool = False

    # Network settings - Robot
    robot_ip: str = "10.0.31.11"
    robot_port: int = 5555
    robot_translator: str = "spot"

    # Timeouts (seconds)
    servo_response_timeout: float = 1.0
    robot_response_timeout: float = 0.5
    socket_error_backoff: float = 5.0

    # Logging
    log_level: str = "INFO"
    log_format: str = "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
    log_file: Optional[str] = None

    # Telemetry - InfluxDB
    telemetry_enabled: bool = False
    influxdb_host: str = "localhost:8086"
    influxdb_database: str = "robot_telemetry"
    influxdb_token: str = ""

    # Performance
    socket_buffer_size: int = 8192
    max_consecutive_errors: int = 3

    def validate(self):
        """
        Validate configuration values.

        Raises:
            ConfigurationError: If configuration is invalid
        """
        # Validate ports
        if not (1 <= self.ingest_port <= 65535):
            raise ConfigurationError(f"Invalid ingest_port: {self.ingest_port}")
        if not (1 <= self.servo_port <= 65535):
            raise ConfigurationError(f"Invalid servo_port: {self.servo_port}")
        if not (1 <= self.robot_port <= 65535):
            raise ConfigurationError(f"Invalid robot_port: {self.robot_port}")

        # Validate timeouts
        if self.servo_response_timeout <= 0:
            raise ConfigurationError(f"Invalid servo_response_timeout: {self.servo_response_timeout}")
        if self.robot_response_timeout <= 0:
            raise ConfigurationError(f"Invalid robot_response_timeout: {self.robot_response_timeout}")

        # Validate log level
        valid_levels = ['DEBUG', 'INFO', 'WARNING', 'ERROR', 'CRITICAL']
        if self.log_level.upper() not in valid_levels:
            raise ConfigurationError(f"Invalid log_level: {self.log_level}. Must be one of {valid_levels}")

        # Validate servo translator
        valid_servo_translators = ['tg_drives']
        if self.servo_translator not in valid_servo_translators:
            raise ConfigurationError(
                f"Invalid servo_translator: {self.servo_translator}. Must be one of {valid_servo_translators}"
            )

        # Validate robot translator
        valid_robot_translators = ['spot']
        if self.robot_translator not in valid_robot_translators:
            raise ConfigurationError(
                f"Invalid robot_translator: {self.robot_translator}. Must be one of {valid_robot_translators}"
            )

    @classmethod
    def from_yaml(cls, path: str) -> 'RelayConfig':
        """
        Load configuration from YAML file.

        Args:
            path: Path to YAML config file

        Returns:
            RelayConfig instance

        Raises:
            ConfigurationError: If file cannot be loaded or parsed
        """
        try:
            with open(path, 'r') as f:
                data = yaml.safe_load(f)

            if not data:
                return cls()

            # Extract nested values
            config_dict = {}

            if 'network' in data:
                if 'ingest' in data['network']:
                    config_dict['ingest_host'] = data['network']['ingest'].get('host', cls.ingest_host)
                    config_dict['ingest_port'] = data['network']['ingest'].get('port', cls.ingest_port)

                if 'servo' in data['network']:
                    config_dict['servo_ip'] = data['network']['servo'].get('ip', cls.servo_ip)
                    config_dict['servo_port'] = data['network']['servo'].get('port', cls.servo_port)
                    config_dict['servo_response_timeout'] = data['network']['servo'].get(
                        'response_timeout', cls.servo_response_timeout
                    )
                    config_dict['servo_translator'] = data['network']['servo'].get(
                        'translator', cls.servo_translator
                    )

                if 'robot' in data['network']:
                    config_dict['robot_ip'] = data['network']['robot'].get('ip', cls.robot_ip)
                    config_dict['robot_port'] = data['network']['robot'].get('port', cls.robot_port)
                    config_dict['robot_response_timeout'] = data['network']['robot'].get(
                        'response_timeout', cls.robot_response_timeout
                    )
                    config_dict['robot_translator'] = data['network']['robot'].get(
                        'translator', cls.robot_translator
                    )

            if 'logging' in data:
                config_dict['log_level'] = data['logging'].get('level', cls.log_level)
                config_dict['log_format'] = data['logging'].get('format', cls.log_format)
                config_dict['log_file'] = data['logging'].get('file', cls.log_file)

            if 'telemetry' in data:
                config_dict['telemetry_enabled'] = data['telemetry'].get('enabled', cls.telemetry_enabled)
                config_dict['influxdb_host'] = data['telemetry'].get('influxdb_host', cls.influxdb_host)
                config_dict['influxdb_database'] = data['telemetry'].get('influxdb_database', cls.influxdb_database)
                config_dict['influxdb_token'] = data['telemetry'].get('influxdb_token', cls.influxdb_token)

            if 'performance' in data:
                config_dict['socket_buffer_size'] = data['performance'].get('socket_buffer_size', cls.socket_buffer_size)
                config_dict['max_consecutive_errors'] = data['performance'].get('max_consecutive_errors', cls.max_consecutive_errors)
                config_dict['socket_error_backoff'] = data['performance'].get('socket_error_backoff', cls.socket_error_backoff)

            if 'tg_drives' in data:
                config_dict['tg_azimuth_min'] = data['tg_drives'].get('azimuth_min', cls.tg_azimuth_min)
                config_dict['tg_azimuth_max'] = data['tg_drives'].get('azimuth_max', cls.tg_azimuth_max)
                config_dict['tg_elevation_min'] = data['tg_drives'].get('elevation_min', cls.tg_elevation_min)
                config_dict['tg_elevation_max'] = data['tg_drives'].get('elevation_max', cls.tg_elevation_max)
                config_dict['tg_speed_max'] = data['tg_drives'].get('speed_max', cls.tg_speed_max)
                config_dict['tg_speed_multiplier'] = data['tg_drives'].get('speed_multiplier', cls.tg_speed_multiplier)
                config_dict['tg_filter_alpha'] = data['tg_drives'].get('filter_alpha', cls.tg_filter_alpha)
                config_dict['tg_swap_axes'] = data['tg_drives'].get('swap_axes', cls.tg_swap_axes)
                config_dict['tg_invert_azimuth'] = data['tg_drives'].get('invert_azimuth', cls.tg_invert_azimuth)
                config_dict['tg_invert_elevation'] = data['tg_drives'].get('invert_elevation', cls.tg_invert_elevation)

            return cls(**config_dict)

        except FileNotFoundError:
            raise ConfigurationError(f"Config file not found: {path}")
        except yaml.YAMLError as e:
            raise ConfigurationError(f"Failed to parse YAML config: {e}")
        except Exception as e:
            raise ConfigurationError(f"Failed to load config from {path}: {e}")

    def __str__(self) -> str:
        """String representation for logging."""
        return (
            f"RelayConfig("
            f"ingest={self.ingest_host}:{self.ingest_port}, "
            f"servo={self.servo_ip}:{self.servo_port} (translator={self.servo_translator}), "
            f"robot={self.robot_ip}:{self.robot_port} (translator={self.robot_translator}), "
            f"log_level={self.log_level})"
        )


def load_configuration(config_path: str = "robot_controller/config.yaml") -> RelayConfig:
    """
    Load configuration from YAML file.

    Args:
        config_path: Path to config.yaml file (default: robot_controller/config.yaml)

    Returns:
        Validated RelayConfig

    Raises:
        ConfigurationError: If configuration file is missing or invalid
    """
    # Try default path first if not absolute
    if not os.path.isabs(config_path):
        # Try relative to current directory
        if not os.path.exists(config_path):
            # Try relative to script directory
            script_dir = os.path.dirname(os.path.abspath(__file__))
            config_path = os.path.join(script_dir, "config.yaml")

    config = RelayConfig.from_yaml(config_path)
    config.validate()

    return config
