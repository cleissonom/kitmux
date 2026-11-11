import os
from pathlib import Path
import signal
import subprocess
import time
import unittest

BINARY = Path(os.environ.get("KITMUX_BINARY", Path(__file__).parents[1] / "build/kitmux"))


class ShutdownTests(unittest.TestCase):
    def test_unread_client_output_does_not_delay_owned_process_shutdown(self):
        helper = subprocess.Popen([str(BINARY.parent / "test_runtime_stop")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        child = int(helper.stderr.readline())

        def cleanup():
            if helper.poll() is None:
                try:
                    os.kill(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                helper.kill()
                helper.wait(timeout=3)
            helper.stderr.close()
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                os.kill(child, 0)
            except ProcessLookupError:
                break
            time.sleep(.01)
        else:
            self.fail("Owned upstream remained alive while client output was blocked")
        self.assertEqual(helper.wait(timeout=6), 0)


if __name__ == "__main__":
    unittest.main()
