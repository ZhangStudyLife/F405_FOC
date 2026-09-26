"""Recovery regression tests; no hardware is opened."""
import itertools
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import bench
import devices


class RecoveryTests(unittest.TestCase):
    def test_spinup_drains_usb_with_uart(self):
        link, monitor = Mock(), Mock()
        monitor.health.return_value = {"rpm": 250}
        with patch.object(bench.time, "monotonic", side_effect=itertools.count(0, .05)):
            bench.wait_speed(link, monitor, 250)
        self.assertGreaterEqual(link.listen.call_count, 2)
        self.assertEqual(link.listen.call_count, monitor.health.call_count)

    def test_recovery_closes_stream_during_power_wait_and_clears_after_power(self):
        old, fresh, supply = Mock(), Mock(), Mock()
        supply.state.protection_status = 0
        active = False
        def opened():
            nonlocal active
            active = True
        def closed():
            nonlocal active
            active = False
        fresh.open_session.side_effect = opened
        fresh.close_session.side_effect = closed
        def power(enabled):
            self.assertFalse(active)
        supply.output.side_effect = power
        fresh.listen.return_value = [SimpleNamespace(fault=8)]
        args = SimpleNamespace(sn="test", stlink_sn="probe")
        with patch.object(bench.bench, "find_port", return_value=SimpleNamespace(device="test", serial_number="test")), \
             patch.object(bench.bench, "Link", return_value=fresh), \
             patch.object(bench.bench, "wait_idle") as idle, \
             patch.object(bench.time, "sleep"), patch.object(bench, "reset_board") as reset:
            self.assertIs(bench.recover(old, None, supply, args, 24, "USB timeout"), fresh)
            fresh.send.assert_any_call("clear")
            idle.assert_called_once_with(fresh, threshold=5.0)
            reset.assert_not_called()
            self.assertFalse(active)

    def test_unresponsive_usb_resets_once_and_never_enables_power(self):
        old, fresh, supply = Mock(), Mock(), Mock()
        supply.state.protection_status = 0
        fresh.listen.return_value = []
        with patch.object(bench.bench, "find_port", return_value=SimpleNamespace(device="test", serial_number="test")), \
             patch.object(bench.bench, "Link", return_value=fresh), \
             patch.object(bench, "reset_board") as reset:
            with self.assertRaisesRegex(RuntimeError, "有效遥测"):
                bench.recover(old, None, supply, SimpleNamespace(sn="test"), 24, "USB timeout")
            reset.assert_called_once()
            self.assertTrue(all(call.args == (False,) for call in supply.output.call_args_list))

    def test_state_failure_escalates_to_reset_with_power_off(self):
        old, fresh, supply = Mock(), Mock(), Mock()
        supply.state.protection_status = 0
        fresh.listen.return_value = [SimpleNamespace(fault=0)]
        def reset(args):
            self.assertEqual(supply.output.call_args.args, (False,))
        with patch.object(bench.bench, "find_port", return_value=SimpleNamespace(device="test", serial_number="test")), \
             patch.object(bench.bench, "Link", return_value=fresh), \
             patch.object(bench.bench, "wait_idle", side_effect=[RuntimeError("idle timeout"), True]) as idle, \
             patch.object(bench.time, "sleep"), \
             patch.object(bench, "reset_board", side_effect=reset) as reboot:
            self.assertIs(bench.recover(old, None, supply, SimpleNamespace(sn="test"), 24, "stuck state"), fresh)
            reboot.assert_called_once()
            self.assertEqual(idle.call_count, 2)

    def test_persistent_fault_stops_after_one_reset(self):
        old, fresh, supply = Mock(), Mock(), Mock()
        supply.state.protection_status = 0
        fresh.listen.return_value = [SimpleNamespace(fault=8)]
        with patch.object(bench.bench, "find_port", return_value=SimpleNamespace(device="test", serial_number="test")), \
             patch.object(bench.bench, "Link", return_value=fresh), \
             patch.object(bench.bench, "wait_idle", side_effect=RuntimeError("persistent fault")), \
             patch.object(bench.time, "sleep"), patch.object(bench, "reset_board") as reset:
            with self.assertRaisesRegex(RuntimeError, "persistent fault"):
                bench.recover(old, None, supply, SimpleNamespace(sn="test"), 24, "fault")
            reset.assert_called_once()
            self.assertEqual(supply.output.call_args.args, (False,))

    def test_failed_power_off_prevents_reset(self):
        old, fresh, supply = Mock(), Mock(), Mock()
        supply.state.protection_status = 0
        supply.output.side_effect = [None, None, RuntimeError("power off failed")]
        fresh.listen.return_value = [SimpleNamespace(fault=0)]
        with patch.object(bench.bench, "find_port", return_value=SimpleNamespace(device="test", serial_number="test")), \
             patch.object(bench.bench, "Link", return_value=fresh), \
             patch.object(bench.bench, "wait_idle", side_effect=RuntimeError("idle timeout")), \
             patch.object(bench.time, "sleep"), patch.object(bench, "reset_board") as reset:
            with self.assertRaisesRegex(RuntimeError, "power off failed"):
                bench.recover(old, None, supply, SimpleNamespace(sn="test"), 24, "fault")
            reset.assert_not_called()

    def test_supply_requires_fresh_full_status_and_output(self):
        supply = devices.StudentPower.__new__(devices.StudentPower)
        supply.running = True
        supply.last_full_ns = time.monotonic_ns()
        supply.state = SimpleNamespace(protection_status=0, output_enabled=False)
        with self.assertRaisesRegex(RuntimeError, "意外关闭"):
            supply.health()
        supply.health(require_output=False)
        supply.last_full_ns = 0
        with self.assertRaisesRegex(RuntimeError, "通信中断"):
            supply.health(require_output=False)


if __name__ == "__main__":
    unittest.main()
