"""
Robot protocol translators.

Translators convert VR client robot control messages into robot-specific protocols.
"""

from .base import RobotTranslator
from .asgard import AsgardTranslator

__all__ = ['RobotTranslator', 'AsgardTranslator']
