#!/bin/sh
# ARM64 Linux exit-time leak scans distort protocol deadlines. Check representative
# non-timed allocation paths separately; the complete suite still uses ASan/UBSan.
set -eu
[ "$#" -eq 1 ] || { echo 'Usage: check-leaks.sh SANITIZED_BINARY' >&2; exit 1; }
binary_dir=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)
export KITMUX_BINARY="$binary_dir/$(basename -- "$1")"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONPATH="$root/tests"
export ASAN_OPTIONS=detect_leaks=1
"${PYTHON:-python3}" -m unittest -v \
    test_import_setup.ImportSetupTests.test_import_merges_same_native_name_with_per_client_execution_variants \
    test_skills.SkillTests.test_update_preview_apply_and_rollback \
    test_subagents.SubagentTests.test_edit_preserves_policy_and_duplicate_starts_unassigned \
    test_mcp_descriptions.McpDescriptionTests.test_add_and_metadata_only_edit_accept_markdown_and_preserve_execution \
    test_tui_details.ResourceDetailsTests.test_installed_skill_document_beyond_32k_can_be_read_to_end
