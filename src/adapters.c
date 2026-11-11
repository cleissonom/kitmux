#include "adapters.h"
#include <stdlib.h>
#include <string.h>

static bool configured(AdRun *run, const char *agent) {
    json_object *managed=km_get(km_get(run->state,"managed_agents"),agent);
    if (managed) return json_object_get_boolean(managed);
    json_object *files=km_get(run->state,"files");
    json_object_object_foreach(files,path,record) {
        (void)path; const char *owner=km_string(record,"agent");
        if (owner && !strcmp(owner,agent)) return true;
    }
    return false;
}

static int selected(Km *km, const char *csv, bool flags[KM_AGENT_COUNT]) {
    for (int i=0;i<KM_AGENT_COUNT;i++) flags[i]=!csv;
    if (!csv) return 0;
    char *copy=strdup(csv), *save=NULL;
    if (!copy) return km_error(km,"out of memory");
    for (char *p=strtok_r(copy,",",&save);p;p=strtok_r(NULL,",",&save)) {
        if (!km_valid_agent(p)) { free(copy); return km_error(km,"unknown client in adapter selection"); }
        for (int i=0;i<KM_AGENT_COUNT;i++) if (!strcmp(p,km_agents[i])) flags[i]=true;
    }
    free(copy); return 0;
}

static int doctor(AdRun *run, bool flags[KM_AGENT_COUNT]) {
    for (int i=0;i<KM_AGENT_COUNT;i++) {
        if (!flags[i]) continue;
        const char *agent=km_agents[i]; AdFile *file=ad_native_file(run,agent);
        if (!file) return -1;
        if (file->toml) {
            json_object *ids=ad_toml_ids(run->km,file->text);
            if (!ids) return -1;
            json_object_put(ids);
        }
        printf("%s: %s config valid; runtime unverified; refresh may require a new session\n",agent,file->existed ? "existing" : "no");
    }
    if (json_object_get_boolean(km_get(run->state,"pending")))
        puts("Interrupted native sync detected; retry sync --apply to recover or inspect conflicts.");
    puts("Scope: user configuration. Project/organization overrides remain native and may restrict availability.");
    return 0;
}

int km_adapter_command(Km *km, int argc, char **argv) {
    if (!argc) return km_error(km,"expected adapter command");
    const char *verb=argv[0], *csv=NULL;
    bool apply=false, apply_requested=false, check=false, configured_only=false;
    bool subagents_only=false, refresh=false, flags[KM_AGENT_COUNT];
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--apply")) { apply=true; apply_requested=true; }
        else if (!strcmp(argv[i],"--preview")) apply=false;
        else if (!strcmp(argv[i],"--configured")) configured_only=true;
        else if (!strcmp(argv[i],"--subagents")) subagents_only=true;
        else if (!strcmp(argv[i],"--refresh")) refresh=true;
        else if (!strcmp(argv[i],"--check")) check=true;
        else if ((!strcmp(argv[i],"--agent") || !strcmp(argv[i],"--from")) && i+1<argc) csv=argv[++i];
        else return km_error(km,"unknown adapter argument");
    }
    if (strcmp(verb,"sync") && strcmp(verb,"import") && strcmp(verb,"restore") && strcmp(verb,"doctor"))
        return km_error(km,"unknown adapter command");
    if (subagents_only && strcmp(verb,"import")) return km_error(km,"subagent selection requires import");
    if (refresh && strcmp(verb,"import")) return km_error(km,"--refresh requires explicit import");
    if (check && (strcmp(verb,"sync") || apply_requested))
        return km_error(km,"Use sync --check without --apply");
    if (selected(km,csv,flags)<0) return -1;
    AdRun run;
    if (ad_open(&run,km,apply)<0) { ad_cleanup(&run); return -1; }
    run.refresh=refresh;
    run.check=check;
    run.management=json_object_new_object();
    int result=0;
    if (!strcmp(verb,"doctor")) result=doctor(&run,flags);
    else {
        for (int i=0;i<KM_AGENT_COUNT;i++) {
            if (!flags[i] || (configured_only && !configured(&run,km_agents[i]))) continue;
            const char *agent=km_agents[i];
            if (!strcmp(verb,"import")) {
                if (!subagents_only) result=ad_import(&run,agent);
                if (result==0 && !subagents_only) result=ad_import_skills(&run,agent);
                if (result==0) result=ad_import_subagents(&run,agent);
            }
            else if (!strcmp(verb,"restore")) {
                result=ad_restore(&run,agent);
                if (result==0) result=ad_restore_links(&run,agent);
                if (result==0) result=ad_restore_subagents(&run,agent);
            } else {
                result=ad_sync_mcp(&run,agent);
                if (result==0) result=ad_sync_skills(&run,agent);
                if (result==0) result=ad_sync_subagents(&run,agent);
            }
            if (result<0) break;
            json_object_object_add(run.management,agent,json_object_new_boolean(strcmp(verb,"restore")!=0));
        }
        /* Never claim native ownership without the desired imported resources:
         * a later sync could otherwise remove entries after a failed save. */
        if (result==0 && apply && !strcmp(verb,"import")) result=km_save(km);
        if (result==0) result=ad_finish(&run);
    }
    ad_cleanup(&run); return result;
}
