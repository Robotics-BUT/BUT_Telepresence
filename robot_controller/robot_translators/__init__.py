"""
Robot protocol translators.

Translators convert VR client robot control messages into robot-specific protocols.
"""

from .base import RobotTranslator
from .spot import SpotTranslator

__all__ = ['RobotTranslator', 'SpotTranslator']
