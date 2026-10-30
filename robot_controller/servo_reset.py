"""
Show the camera-head servo drive's health and clear its latched faults, through the servo translator config.yaml names.

    python3 -m robot_controller.servo_reset            # show the health, reset if a fault is latched
    python3 -m robot_controller.servo_reset --status   # only show the health
    python3 -m robot_controller.servo_reset --force    # reset even when no fault is reported

Safe to run while the relay service is running. What a fault is and how it is cleared is the translator's business
(ServoTranslator.health / reset_faults); a translator that cannot observe its drive or reset it says so.
"""

import argparse
import logging
import sys
import time

from .config import load_configuration
from .relay_service import create_servo_translator


def show(health) -> None:
    if health is None:
        print("this servo translator cannot observe its drive")
    elif not health.answering:
        print("the servo drive does not answer")
    elif health.fault:
        print(f"fault: {health.description}")
    else:
        print("healthy, no fault latched")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().split("\n\n")[0])
    parser.add_argument("--config", default="robot_controller/config.yaml")
    parser.add_argument("--status", action="store_true", help="only show the health")
    parser.add_argument("--force", action="store_true", help="reset even when no fault is reported")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    servo = create_servo_translator(load_configuration(args.config))
    if not servo.initialize():
        print("could not reach the servo driver")
        return 1
    try:
        health = servo.health()
        show(health)
        if args.status:
            return 0
        if not servo.supports_fault_reset():
            print("this servo translator cannot reset faults")
            return 1
        if not args.force and (health is None or not health.fault):
            print("nothing to reset (--force to reset anyway)")
            return 0
        servo.reset_faults()
        time.sleep(0.3)
        after = servo.health()
        show(after)
        return 0 if after is not None and after.answering and not after.fault else 2
    finally:
        servo.close()


if __name__ == "__main__":
    sys.exit(main())
