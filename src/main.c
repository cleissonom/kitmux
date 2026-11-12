#include "kitmux.h"
#include <locale.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void km_help(FILE *out) {
    fprintf(out,
        "kitmux %s — MCPs, skills, and subagents, assigned to your clients\n\n"
        "Usage: kitmux [--home DIR] [--no-color] COMMAND\n"
        "Run without a command in a terminal for the interactive interface.\n\n"
        "  mcp|skill|subagent group add|list|show|rename|remove ID\n"
        "  mcp|skill|subagent group include|exclude GROUP RESOURCE_ID ...\n"
        "  mcp|skill|subagent group assign GROUP --agents codex,claude,opencode\n"
        "  mcp|skill|subagent group enable|disable GROUP\n"
        "  group migrate --preview|--apply\n"
        "  mcp add|edit ID --command PROGRAM [--mode managed] -- ARGS...\n"
        "  mcp add|edit ID --url URL\n"
        "  mcp add|edit ID --from FILE --format codex|claude|opencode\n"
        "  mcp add|edit ID [--description TEXT | --description-file FILE]\n"
        "  mcp list|show|remove ID\n"
        "  mcp enable|disable|inherit ID [--agent AGENT]\n"
        "  mcp reconnect ID --agent AGENT\n"
        "  mcp status ID --agent AGENT [--json]\n"
        "  skill add DIRECTORY|REPOSITORY [--path SUBDIR] [--ref REV]\n"
        "  skill list|show|remove ID\n"
        "  skill enable|disable|inherit ID [--agent AGENT]\n"
        "  skill update ID [--source DIRECTORY] [--preview|--apply]\n"
        "  skill rollback ID\n"
        "  subagent add|edit ID --from FILE [--format FORMAT] [--description TEXT]\n"
        "  subagent duplicate ID NEW_ID\n"
        "  subagent list [--json]    subagent show ID [--json]\n"
        "  subagent remove ID    subagent enable|disable|inherit ID [--agent AGENT]\n"
        "  subagent import --from codex,claude,opencode --preview|--apply\n"
        "  import [--refresh] --from codex,claude,opencode --preview|--apply\n"
        "  sync --preview|--apply|--check [--agent AGENT]\n"
        "  restore --preview|--apply [--agent AGENT]\n"
        "  status [--json]    doctor    --version    --help\n\n"
        "Use sync --preview/--apply to adopt native configurations initially.\n"
        "Later catalog changes automatically sync clients already adopted.\n"
        "Native changes may need an agent reload. Managed bridges observe policy.\n",
        KM_VERSION);
}

static bool unlocked_command(int argc, char **argv) {
    if (!argc || !strcmp(argv[0], "status") || !strcmp(argv[0], "doctor") ||
        !strcmp(argv[0], "help") || !strcmp(argv[0], "bridge")) return true;
    if (argc > 1 && (!strcmp(argv[0], "mcp") || !strcmp(argv[0], "skill") || !strcmp(argv[0], "subagent")) && !strcmp(argv[1], "group"))
        return argc == 2 || !strcmp(argv[2], "list") || !strcmp(argv[2], "show");
    if (argc == 3 && !strcmp(argv[0], "group") && !strcmp(argv[1], "migrate")) return !strcmp(argv[2], "--preview");
    if (argc > 1 && !strcmp(argv[0], "skill") && !strcmp(argv[1], "update")) {
        for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--apply")) return false;
        return true;
    }
    if (argc > 1 && !strcmp(argv[0], "subagent") && !strcmp(argv[1], "import")) {
        for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--apply")) return false;
        return true;
    }
    if (argc > 1 && (!strcmp(argv[0], "mcp") || !strcmp(argv[0], "skill") || !strcmp(argv[0], "group") || !strcmp(argv[0], "subagent")))
        return !strcmp(argv[1], "list") || !strcmp(argv[1], "show") ||
               !strcmp(argv[1], "reconnect") || !strcmp(argv[1], "status");
    if ((!strcmp(argv[0], "import") || !strcmp(argv[0], "sync") || !strcmp(argv[0], "restore"))) {
        for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--apply")) return false;
        return true;
    }
    return false;
}

static int route(Km *km, int argc, char **argv) {
    if (!argc || !strcmp(argv[0], "help")) { km_help(stdout); return 0; }
    if (argc > 1 && (!strcmp(argv[0], "mcp") || !strcmp(argv[0], "skill") || !strcmp(argv[0], "subagent")) && !strcmp(argv[1], "group"))
        return km_scoped_group_command(km, argv[0], argc - 2, argv + 2);
    if (!strcmp(argv[0], "group")) return km_group_command(km, argc - 1, argv + 1);
    if (!strcmp(argv[0], "mcp")) return km_mcp_command(km, argc - 1, argv + 1);
    if (!strcmp(argv[0], "skill")) return km_skill_command(km, argc - 1, argv + 1);
    if (!strcmp(argv[0], "subagent")) return km_subagent_command(km, argc - 1, argv + 1);
    if (!strcmp(argv[0], "bridge")) return km_runtime_command(km, argc, argv);
    if (!strcmp(argv[0], "status") && (argc == 1 || (argc == 2 && !strcmp(argv[1], "--json")))) return km_status(km, argc == 2);
    if (!strcmp(argv[0], "sync") || !strcmp(argv[0], "import") || !strcmp(argv[0], "restore") || !strcmp(argv[0], "doctor"))
        return km_adapter_command(km, argc, argv);
    return km_error(km, "Unknown command or arguments; run kitmux --help");
}

static int sync_configured(Km *km) {
    char path[PATH_MAX];
    struct stat st;
    if (km_path(km, path, sizeof(path), "%s/adapters.json", km->state) < 0) return -1;
    if (lstat(path, &st) < 0 && errno == ENOENT) return 0;
    char *args[] = {"sync", "--apply", "--configured"};
    if (km_adapter_command(km, 3, args) == 0) return 0;
    char cause[sizeof(km->error)];
    snprintf(cause, sizeof(cause), "%s", km->error);
    return km_error(km, "Catalog saved; native sync pending: %.850s. Resolve the conflict and run sync --apply.", cause);
}

int km_dispatch(Km *km, int argc, char **argv) {
    km->error[0] = '\0';
    bool locked = !unlocked_command(argc, argv);
    if (locked && km_lock(km) < 0) return -1;
    int result = km_load(km);
    int64_t before = json_object_get_int64(km_get(km->catalog, "revision"));
    if (!result) result = route(km, argc, argv);
    if (!result && locked && argc && (!strcmp(argv[0], "group") || !strcmp(argv[0], "mcp") || !strcmp(argv[0], "skill") ||
        (!strcmp(argv[0], "subagent") && argc > 1 && strcmp(argv[1], "import"))) &&
        before != json_object_get_int64(km_get(km->catalog, "revision"))) result = sync_configured(km);
    if (locked) km_unlock(km);
    return result;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *home = NULL;
    int start = 1;
    while (start < argc && !strncmp(argv[start], "--", 2)) {
        const char *option = argv[start++];
        if (!strcmp(option, "--help")) { km_help(stdout); return 0; }
        if (!strcmp(option, "--version")) { puts("kitmux " KM_VERSION); return 0; }
        if (!strcmp(option, "--no-color")) { setenv("NO_COLOR", "1", 1); continue; }
        if (!strcmp(option, "--home") && start < argc) { home = argv[start++]; continue; }
        fputs("kitmux: invalid global option; run --help\n", stderr); return 2;
    }
    if (start == argc && !isatty(STDIN_FILENO)) { km_help(stdout); return 0; }
    Km km;
    if (km_init(&km, home) < 0) { fprintf(stderr, "kitmux: %s\n", km.error); return 1; }
    int result;
    if (start == argc) { result = km_load(&km); if (!result) result = km_tui(&km); }
    else result = km_dispatch(&km, argc - start, argv + start);
    if (result < 0) fprintf(stderr, "kitmux: %s\n", *km.error ? km.error : "Operation failed");
    km_close(&km);
    return result < 0 ? 1 : result;
}
