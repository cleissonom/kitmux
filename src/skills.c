#include "skills_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *subdir;
    const char *ref;
    bool apply;
} Options;

static int options(Km *km, int argc, char **argv, bool review, Options *out) {
    memset(out, 0, sizeof(*out));
    bool mode_seen = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--apply") || !strcmp(argv[i], "--preview")) {
            if (!review || mode_seen) return km_error(km, "choose one of --preview or --apply for skill update");
            out->apply = !strcmp(argv[i], "--apply");
            mode_seen = true;
        }
        else if (!strcmp(argv[i], "--path") && i + 1 < argc) out->subdir = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) out->ref = argv[++i];
        else return km_error(km, "unknown or incomplete skill option: %s", argv[i]);
    }
    return 0;
}

static json_object *copy_object(Km *km, json_object *source) {
    json_object *copy = NULL;
    if (json_object_deep_copy(source, &copy, NULL)) km_error(km, "cannot copy skill metadata");
    return copy;
}

static void package_fields(json_object *object, const KmSkill *skill, const char *path) {
    json_object_object_add(object, "path", json_object_new_string(path));
    json_object_object_add(object, "source", json_object_new_string(skill->source));
    json_object_object_add(object, "revision", json_object_new_string(skill->revision));
    json_object_object_add(object, "subdir", json_object_new_string(skill->subdir));
    json_object_object_add(object, "ref", json_object_new_string(skill->ref));
    json_object_object_add(object, "hash", json_object_new_string(skill->hash));
    json_object_object_add(object, "description", json_object_new_string(skill->description));
    json_object_object_add(object, "compatibility", json_object_new_string(skill->compatibility));
    json_object_object_add(object, "license", json_object_new_string(skill->license));
}

static int install_new(Km *km, KmSkill *skill) {
    json_object *resources = km_resources(km, "skill");
    if (km_get(resources, skill->name)) return km_error(km, "skill already exists; use skill update");
    char path[PATH_MAX];
    if (km_skill_store(km, skill, path, sizeof(path))) return -1;
    json_object *object = json_object_new_object();
    package_fields(object, skill, path);
    json_object_object_add(object, "enabled", json_object_new_boolean(true));
    json_object_object_add(object, "agents", json_object_new_object());
    json_object_object_add(resources, skill->name, object);
    if (km_save(km)) return -1;
    printf("Installed %s at %s. Unassigned; choose a group or agent to enable it.\n", skill->name, skill->revision);
    return 0;
}

static int skill_add(Km *km, int argc, char **argv) {
    if (argc < 2) return km_error(km, "usage: kitmux skill add SOURCE [--path SUBDIR] [--ref REVISION]");
    Options choice;
    if (options(km, argc, argv, false, &choice)) return -1;
    KmSkill skill;
    int result = km_skill_acquire(km, argv[1], choice.subdir, choice.ref, &skill);
    if (!result) result = km_skill_inspect(km, &skill);
    if (!result) result = install_new(km, &skill);
    km_skill_cleanup(&skill);
    return result;
}

static int apply_update(Km *km, const char *name, json_object *existing, KmSkill *skill) {
    char path[PATH_MAX];
    if (km_skill_store(km, skill, path, sizeof(path))) return -1;
    json_object *replacement = copy_object(km, existing);
    if (!replacement) return -1;
    json_object *previous = copy_object(km, existing);
    if (!previous) { json_object_put(replacement); return -1; }
    json_object_object_del(previous, "previous");
    package_fields(replacement, skill, path);
    json_object_object_add(replacement, "previous", previous);
    json_object_object_add(km_resources(km, "skill"), name, replacement);
    if (km_save(km)) return -1;
    printf("Updated %s to %s.\n", name, skill->revision);
    return 0;
}

static int inspect_update(Km *km, const char *name, json_object *existing,
                           Options *choice, KmSkill *skill) {
    const char *source = km_string(existing, "source");
    if (!source) return km_error(km, "skill has no update source");
    const char *subdir = choice->subdir ? choice->subdir : km_string(existing, "subdir");
    const char *ref = choice->ref ? choice->ref : km_string(existing, "ref");
    int result = km_skill_acquire(km, source, subdir, ref && *ref ? ref : NULL, skill);
    if (!result) result = km_skill_inspect(km, skill);
    if (!result && strcmp(name, skill->name)) result = km_error(km, "updated skill name changed; install it as a separate skill");
    return result;
}

static int skill_update(Km *km, int argc, char **argv) {
    if (argc < 2) return km_error(km, "usage: kitmux skill update NAME [--preview|--apply] [--ref REVISION]");
    json_object *existing = km_get(km_resources(km, "skill"), argv[1]);
    if (!existing) return km_error(km, "unknown skill: %s", argv[1]);
    Options choice;
    if (options(km, argc, argv, true, &choice)) return -1;
    KmSkill skill = {0};
    int result = inspect_update(km, argv[1], existing, &choice, &skill);
    const char *previous_hash = km_string(existing, "hash");
    const char *old_path = km_string(existing, "path");
    const char *old_revision = km_string(existing, "revision");
    const char *old_ref = km_string(existing, "ref");
    const char *old_subdir = km_string(existing, "subdir");
    if (!result && (!previous_hash || !old_path || !old_revision)) result = km_error(km, "skill has incomplete installation metadata");
    bool unchanged = !result && !strcmp(previous_hash, skill.hash) && !strcmp(old_revision, skill.revision) &&
                     !strcmp(old_ref ? old_ref : "", skill.ref) && !strcmp(old_subdir ? old_subdir : "", skill.subdir);
    if (unchanged) {
        printf("%s is already current (%s).\n", argv[1], skill.revision);
    } else if (!result && choice.apply) {
        result = apply_update(km, argv[1], existing, &skill);
    } else if (!result) {
        printf("Update preview: %s\n  %s -> %s\n", argv[1], old_revision, skill.revision);
        result = km_skill_diff(km, old_path, skill.root);
        if (!result) puts("Use --apply to install this revision. Catalog and projections are unchanged.");
    }
    km_skill_cleanup(&skill);
    return result;
}

static int skill_rollback(Km *km, int argc, char **argv) {
    if (argc != 2) return km_error(km, "usage: kitmux skill rollback NAME");
    json_object *existing = km_get(km_resources(km, "skill"), argv[1]);
    json_object *previous = existing ? km_get(existing, "previous") : NULL;
    if (!previous) return km_error(km, "skill has no previous installed revision");
    const char *path = km_string(previous, "path");
    const char *expected = km_string(previous, "hash");
    KmSkill check = {0};
    if (!path || !expected || km_path(km, check.root, sizeof(check.root), "%s", path) ||
        km_skill_inspect(km, &check)) return -1;
    if (strcmp(expected, check.hash)) return km_error(km, "previous skill package was modified");
    json_object *replacement = copy_object(km, previous);
    json_object *next_previous = copy_object(km, existing);
    if (!replacement || !next_previous) { json_object_put(replacement); json_object_put(next_previous); return -1; }
    json_object_object_del(next_previous, "previous");
    json_object_object_add(replacement, "previous", next_previous);
    json_object *enabled = km_get(existing, "enabled"), *agents = km_get(existing, "agents");
    json_object_object_add(replacement, "enabled", enabled ? json_object_get(enabled) : json_object_new_boolean(true));
    json_object_object_add(replacement, "agents", agents ? json_object_get(agents) : json_object_new_object());
    json_object_object_add(km_resources(km, "skill"), argv[1], replacement);
    if (km_save(km)) return -1;
    printf("Rolled back %s.\n", argv[1]);
    return 0;
}

static int skill_list(Km *km, int argc, char **argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--json"))) return km_error(km, "usage: kitmux skill list [--json]");
    json_object *resources = km_resources(km, "skill");
    if (argc == 2) puts(json_object_to_json_string_ext(resources, JSON_C_TO_STRING_PRETTY));
    else {
        puts("SKILL                         INSTALLED REVISION");
        json_object_object_foreach(resources, name, value) {
            printf("%-29s %.64s\n", name, km_string(value, "revision") ? km_string(value, "revision") : "unknown");
        }
    }
    return 0;
}

int km_skill_command(Km *km, int argc, char **argv) {
    if (!argc || !strcmp(argv[0], "list")) return skill_list(km, argc, argv);
    if (!strcmp(argv[0], "add")) return skill_add(km, argc, argv);
    if (!strcmp(argv[0], "update")) return skill_update(km, argc, argv);
    if (!strcmp(argv[0], "rollback")) return skill_rollback(km, argc, argv);
    if (!strcmp(argv[0], "enable") || !strcmp(argv[0], "disable") || !strcmp(argv[0], "inherit"))
        return km_resource_toggle(km, "skill", argc, argv);
    if (!strcmp(argv[0], "remove") && argc == 2) return km_remove_resource(km, "skill", argv[1]);
    if (!strcmp(argv[0], "show") && (argc == 2 || (argc == 3 && !strcmp(argv[2], "--json")))) {
        json_object *resource = km_get(km_resources(km, "skill"), argv[1]);
        if (!resource) return km_error(km, "unknown skill: %s", argv[1]);
        puts(json_object_to_json_string_ext(resource, JSON_C_TO_STRING_PRETTY));
        return 0;
    }
    return km_error(km, "usage: kitmux skill add|list|show|enable|disable|inherit|update|rollback|remove");
}
