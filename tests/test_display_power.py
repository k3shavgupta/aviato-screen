import importlib.util
from datetime import datetime, timezone
from pathlib import Path
import unittest
from tempfile import TemporaryDirectory
from unittest.mock import patch
from zoneinfo import ZoneInfo


spec = importlib.util.spec_from_file_location(
    "display_power", Path(__file__).parents[1] / "display-power.py"
)
power = importlib.util.module_from_spec(spec)
spec.loader.exec_module(power)


class DisplayScheduleTests(unittest.TestCase):
    def test_adaptive_keeps_the_dim_clock_visible_overnight(self):
        for hour in range(24):
            instant = datetime(2026, 11, 8, hour, tzinfo=ZoneInfo("Asia/Kolkata"))
            self.assertTrue(power.display_should_be_on(instant, "adaptive"))
            for profile in ("minimal", "focus", "album", "ambient", "legacy", "unknown"):
                self.assertEqual(power.display_should_be_on(instant, profile), 8 <= hour < 23)

    def test_power_controller_reads_the_saved_profile(self):
        with TemporaryDirectory() as directory, patch.object(power.Path, "home", return_value=Path(directory)):
            self.assertEqual(power.current_profile(), "minimal")
            profile = Path(directory) / ".config/aviato-screen/profile"
            profile.parent.mkdir(parents=True)
            profile.write_text("adaptive\n")
            self.assertEqual(power.current_profile(), "adaptive")
            profile.write_text("minimal\n")
            self.assertEqual(power.current_profile(), "minimal")

    def test_india_time_boundaries(self):
        for hour, minute, expected in (
            (0, 0, False), (7, 59, False), (8, 0, True),
            (12, 0, True), (22, 59, True), (23, 0, False), (23, 59, False),
        ):
            with self.subTest(hour=hour, minute=minute):
                instant = datetime(2026, 11, 6, hour, minute, tzinfo=ZoneInfo("Asia/Kolkata"))
                self.assertEqual(power.display_should_be_on(instant), expected)

    def test_utc_inputs_use_india_time(self):
        self.assertTrue(power.display_should_be_on(datetime(2026, 11, 6, 2, 30, tzinfo=timezone.utc)))
        self.assertTrue(power.display_should_be_on(datetime(2026, 11, 6, 17, 29, tzinfo=timezone.utc)))
        self.assertFalse(power.display_should_be_on(datetime(2026, 11, 6, 17, 30, tzinfo=timezone.utc)))


if __name__ == "__main__":
    unittest.main()
