#ifndef KITMUX_SKILLS_INTERNAL_H
#define KITMUX_SKILLS_INTERNAL_H
#include "kitmux.h"

typedef struct {
    char root[PATH_MAX];
    char temporary[PATH_MAX];
    char source[PATH_MAX];
    char subdir[PATH_MAX];
    char ref[129];
    char revision[129];
    char name[65];
    char description[1025];
    char compatibility[501];
    char license[1025];
    char hash[65];
} KmSkill;

int km_skill_acquire(Km *km, const char *source, const char *subdir,
                     const char *ref, KmSkill *skill);
int km_skill_inspect(Km *km, KmSkill *skill);
int km_skill_store(Km *km, KmSkill *skill, char *out, size_t size);
void km_skill_cleanup(KmSkill *skill);
int km_skill_remove_tree(const char *path);
int km_skill_git_package(Km *km, KmSkill *skill);
int km_skill_diff(Km *km, const char *old_root, const char *new_root);
#endif
