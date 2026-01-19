"""
Servo translators for different robot types.

Each robot may use a different servo driver with a different protocol.
This package provides translators for different servo driver types.
"""

from .base import ServoTranslator
from .tg_drives import TGDrivesTranslator

__all__ = ['ServoTranslator', 'TGDrivesTranslator']
