"""Drive supervision without hardware: the base class's watching and reporting, the TG translator's health in words,
and the relay's servo status packet (0x08).   python3 -m unittest robot_controller.tests.test_servo_supervision"""

import struct
import time
import types
import unittest

from robot_controller.relay_service import UDPRelayService
from robot_controller.servo_translators import ServoHealth, ServoTranslator, TGDrivesTranslator


class Scripted(ServoTranslator):
    """A translator whose health follows a list, one entry per supervision step (the last one repeats)."""
    SUPERVISION_PERIOD_S = 0.01
    SUPERVISION_REPORT_S = 0.2

    def __init__(self, script, resettable=True):
        super().__init__("127.0.0.1", 0, 0.1)
        self.script, self.resettable, self.resets = list(script), resettable, 0

    def initialize(self): return True
    def translate_and_forward(self, data, client_addr): return None
    def close(self): pass
    def health(self): return self.script.pop(0) if len(self.script) > 1 else self.script[0]
    def supports_fault_reset(self): return self.resettable
    def reset_faults(self):
        self.resets += 1
        return True


class Blind(ServoTranslator):
    """A translator that cannot observe its drive (the defaults)."""
    def initialize(self): return True
    def translate_and_forward(self, data, client_addr): return None
    def close(self): pass


OK = ServoHealth(answering=True)
FAULT = ServoHealth(answering=True, fault=True, description="Position (both axes)")
SILENT = ServoHealth(answering=False)


class SupervisionTest(unittest.TestCase):
    def watch(self, servo, seconds):
        reports = []
        servo.start_supervision(lambda h: reports.append((time.monotonic(), h)))
        time.sleep(seconds)
        servo.stop_supervision()
        return reports

    def test_change_is_reported_at_once(self):
        reports = self.watch(Scripted([OK] * 5 + [FAULT] * 5 + [OK]), 0.5)
        states = [h for _, h in reports]
        self.assertEqual(states[:3], [OK, FAULT, OK])

    def test_unchanged_state_is_reported_periodically(self):
        reports = self.watch(Scripted([OK]), 0.65)
        self.assertGreaterEqual(len(reports), 3)          # at start, then every SUPERVISION_REPORT_S
        gaps = [b[0] - a[0] for a, b in zip(reports, reports[1:])]
        self.assertTrue(all(0.15 < g < 0.3 for g in gaps), gaps)

    def test_fault_and_silence_are_logged(self):
        with self.assertLogs(Scripted.__module__, level="WARNING") as logs:
            self.watch(Scripted([OK, FAULT, FAULT, SILENT]), 0.2)
        text = "\n".join(logs.output)
        self.assertIn("Servo fault: Position (both axes)", text)
        self.assertIn("reset:", text)                     # the hint, since this translator can reset
        self.assertIn("not answering", text)

    def test_blind_translator_is_not_watched(self):
        servo = Blind("127.0.0.1", 0, 0.1)
        self.assertIsNone(servo.health())
        self.assertFalse(servo.supports_fault_reset())
        self.assertFalse(servo.reset_faults())
        self.assertEqual(self.watch(servo, 0.1), [])


class TGHealthTest(unittest.TestCase):
    def health_for(self, err_el, err_az):
        tg = TGDrivesTranslator("127.0.0.1", 9, 0.1)
        tg._drive_state = (time.monotonic(), {"el": (0xb7, err_el), "az": (0xb7, err_az)})
        return tg.health()

    def test_healthy(self):
        self.assertEqual(self.health_for(0, 0), ServoHealth(answering=True))

    def test_fault_on_both_axes_in_words(self):
        h = self.health_for(0x8000, 0x8000)
        self.assertTrue(h.fault)
        self.assertEqual(h.description, "Position (both axes)")

    def test_faults_on_one_axis(self):
        h = self.health_for(0x00200000 | 0x8000, 0)
        self.assertEqual(h.description, "Position+OverCurrent (one axis)")

    def test_resettable(self):
        self.assertTrue(TGDrivesTranslator("127.0.0.1", 9, 0.1).supports_fault_reset())


class StatusPacketTest(unittest.TestCase):
    def packet(self, health, resettable=True):
        sent = []
        relay = types.SimpleNamespace(_headset_addr=("10.0.0.1", 5000),
                                      servo_translator=Scripted([health], resettable=resettable),
                                      ingest_socket=types.SimpleNamespace(sendto=lambda data, addr: sent.append(data)))
        UDPRelayService._send_servo_status(relay, health)
        return sent[0]

    def test_fault_packet(self):
        data = self.packet(FAULT)
        typ, flags, length = struct.unpack_from('<BBB', data)
        self.assertEqual((typ, flags), (0x08, 1 | 2 | 4))
        self.assertEqual(data[3:3 + length].decode(), "Position (both axes)")

    def test_healthy_packet_without_reset(self):
        self.assertEqual(self.packet(OK, resettable=False), bytes([0x08, 1, 0]))


if __name__ == "__main__":
    unittest.main()
