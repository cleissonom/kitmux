#!/bin/sh
# Run isolated existing behavioral contracts against the packaged executable.
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: smoke-release.sh BINARY' >&2; exit 1; }
binary_dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
export KITMUX_BINARY="$binary_dir/$(basename -- "$1")"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONPATH="$root/tests"
unset OPENSSL_MODULES OPENSSL_CONF DYLD_LIBRARY_PATH LD_LIBRARY_PATH
"$KITMUX_BINARY" --version
"$KITMUX_BINARY" --help > /dev/null
python3 -m unittest -v \
    test_import_setup.ImportSetupTests.test_import_merges_same_native_name_with_per_client_execution_variants \
    test_skills.SkillTests.test_local_install_preserves_package_and_starts_unassigned \
    test_runtime.RuntimeTests.test_discovery_call_metadata_and_reconnect \
    test_tui.TerminalTests.test_resize_navigation_and_terminal_restore
python3 - <<'PY'
import tempfile
from test_tui import TerminalSession

for term in ("xterm-256color", "tmux-256color"):
    with tempfile.TemporaryDirectory(prefix="km-release-", dir="/tmp") as home:
        terminal = TerminalSession(home, term=term)
        try:
            terminal.wait_text("1 MCPs")
            terminal.send(b"q")
            assert terminal.wait_exit() == 0
        finally:
            terminal.close()
PY
