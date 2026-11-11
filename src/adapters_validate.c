#include "adapters.h"
#include "subagents.h"
#include <stdlib.h>
#include <string.h>

static bool string_field(json_object *object, const char *key, bool required) {
    json_object *value=km_get(object,key);
    if (!value) return !required;
    const char *text=km_string(object,key);
    return text && strlen(text)==(size_t)json_object_get_string_len(value);
}

static bool file_path(Km *km, const char *path, const char *agent) {
    char expected[PATH_MAX]; const char *relatives[3]={NULL,NULL,NULL};
    if (!strcmp(agent,"codex")) relatives[0]=".codex/config.toml";
    else if (!strcmp(agent,"claude")) { relatives[0]=".claude.json"; relatives[1]=".claude/settings.json"; }
    else if (!strcmp(agent,"opencode")) { relatives[0]=".config/opencode/opencode.json"; relatives[1]=".config/opencode/opencode.jsonc"; }
    else return false;
    for (int i=0;relatives[i];i++) {
        if (km_path(km,expected,sizeof(expected),"%s/%s",km->home,relatives[i])<0) return false;
        if (!strcmp(path,expected)) return true;
    }
    return false;
}

static bool entry_key(const char *agent, const char *path, const char *key) {
    if (!strcmp(agent,"codex")) return km_valid_id(key);
    if (!strcmp(agent,"claude")) {
        bool settings=strlen(path)>14 && !strcmp(path+strlen(path)-14,"/settings.json");
        const char *prefix=settings ? "skillOverrides/" : "mcpServers/";
        return !strncmp(key,prefix,strlen(prefix)) && km_valid_id(key+strlen(prefix));
    }
    return !strcmp(key,"permissions") || (!strncmp(key,"mcp/servers/",12) && km_valid_id(key+12)) ||
           (!strncmp(key,"agents/",7) && km_valid_id(key+7));
}

static bool entry_valid(Km *km, const char *agent, const char *path, const char *key, json_object *entry) {
    if (!json_object_is_type(entry,json_type_object) || !entry_key(agent,path,key)) return false;
    const char *fields[]={"before","after","previous"};
    for (size_t i=0;i<3;i++) {
        if (!string_field(entry,fields[i],false)) return false;
        const char *raw=km_string(entry,fields[i]);
        if (!raw) continue;
        json_object *doc;
        if (!strcmp(agent,"opencode") && !strncmp(key,"agents/",7))
            doc=km_subagent_parse(km,raw,"opencode-json",key+7);
        else doc=!strcmp(agent,"codex") ? ad_toml_parse(km,raw) : ad_parse(km,raw);
        if (!doc) return false;
        json_object_put(doc);
    }
    return true;
}

static bool links_valid(Km *km, json_object *records) {
    if (!records) return true;
    if (!json_object_is_type(records,json_type_object)) return false;
    json_object_object_foreach(records,path,record) {
        const char *agent=km_string(record,"agent"); char prefix[PATH_MAX];
        if (!agent || !km_valid_agent(agent) || !string_field(record,"agent",true)) return false;
        const char *roots[3]={!strcmp(agent,"codex") ? ".agents/skills" : !strcmp(agent,"claude") ? ".claude/skills" : ".config/opencode/skills",NULL,NULL};
        if (!strcmp(agent,"codex")) roots[1]=".codex/skills";
        bool allowed=false;
        for (size_t i=0;roots[i];i++) {
            if (km_path(km,prefix,sizeof(prefix),"%s/%s/",km->home,roots[i])<0) return false;
            if (!strncmp(path,prefix,strlen(prefix)) && km_valid_id(path+strlen(prefix))) allowed=true;
        }
        if (!allowed) return false;
        const char *fields[]={"before","after","previous"};
        for (size_t i=0;i<3;i++) {
            if (!string_field(record,fields[i],false)) return false;
            const char *value=km_string(record,fields[i]);
            if (value && !*value) return false;
        }
    }
    return true;
}

int ad_validate_state(Km *km, json_object *state) {
    json_object *files=km_get(state,"files"), *version=km_get(state,"version"), *pending=km_get(state,"pending");
    if (!json_object_is_type(state,json_type_object) || !json_object_is_type(version,json_type_int) ||
        (json_object_get_int(version)!=1 && json_object_get_int(version)!=2) || !json_object_is_type(files,json_type_object) ||
        (pending && !json_object_is_type(pending,json_type_boolean))) goto invalid;
    json_object_object_foreach(files,path,record) {
        const char *agent=km_string(record,"agent");
        json_object *entries=km_get(record,"entries"), *toml=km_get(record,"toml");
        if (!json_object_is_type(record,json_type_object) || !agent || !km_valid_agent(agent) ||
            !string_field(record,"agent",true) || !file_path(km,path,agent) ||
            !json_object_is_type(entries,json_type_object) || !json_object_is_type(toml,json_type_boolean) ||
            json_object_get_boolean(toml)!=(!strcmp(agent,"codex"))) goto invalid;
        json_object_object_foreach(entries,key,entry) {
            if (!entry_valid(km,agent,path,key,entry)) goto invalid;
            if (!strcmp(agent,"opencode") && !strncmp(key,"agents/",7) && json_object_get_int(version)!=2) goto invalid;
        }
    }
    if (!links_valid(km,km_get(state,"links"))) goto invalid;
    if (ad_subagent_state_valid(km,state)<0) goto invalid;
    json_object *management=km_get(state,"managed_agents");
    if (management) {
        if (!json_object_is_type(management,json_type_object)) goto invalid;
        json_object_object_foreach(management,agent,value)
            if (!km_valid_agent(agent) || !json_object_is_type(value,json_type_boolean)) goto invalid;
    }
    return 0;
invalid:
    return km_error(km,"invalid or unsafe adapter ownership record; restore the private state backup before retrying");
}

static bool strings(json_object *array) {
    if (!json_object_is_type(array,json_type_array)) return false;
    for (size_t i=0;i<json_object_array_length(array);i++) {
        json_object *item=json_object_array_get_idx(array,i);
        if (!json_object_is_type(item,json_type_string) ||
            strlen(json_object_get_string(item))!=(size_t)json_object_get_string_len(item)) return false;
    }
    return true;
}

int ad_server_shape(Km *km, const char *agent, json_object *server) {
    if (!json_object_is_type(server,json_type_object)) goto invalid;
    json_object *command=km_get(server,"command"), *args=km_get(server,"args");
    if (command) {
        if (!strcmp(agent,"opencode")) {
            if (!strings(command) || !json_object_array_length(command)) goto invalid;
        } else if (!string_field(server,"command",true)) goto invalid;
    }
    if (args && !strings(args)) goto invalid;
    const char *text_keys[]={"url","cwd","type"};
    for (size_t i=0;i<3;i++) if (!string_field(server,text_keys[i],false)) goto invalid;
    const char *bool_keys[]={"enabled","disabled"};
    for (size_t i=0;i<2;i++) {
        json_object *value=km_get(server,bool_keys[i]);
        if (value && !json_object_is_type(value,json_type_boolean)) goto invalid;
    }
    const char *maps[]={"env","environment","headers","http_headers"};
    for (size_t i=0;i<4;i++) {
        json_object *map=km_get(server,maps[i]);
        if (!map) continue;
        if (!json_object_is_type(map,json_type_object)) goto invalid;
        json_object_object_foreach(map,key,value) {
            (void)value;
            if (!string_field(map,key,true)) goto invalid;
        }
    }
    return 0;
invalid:
    return km_error(km,"unsupported or malformed native MCP definition for %s",agent);
}

int ad_native_shape(Km *km, const char *agent, const char *path, json_object *doc) {
    if (!strcmp(agent,"claude") && strstr(path,"/settings.json")) {
        json_object *overrides=km_get(doc,"skillOverrides");
        if (overrides && !json_object_is_type(overrides,json_type_object)) goto invalid;
        return 0;
    }
    json_object *servers=km_get(doc,"mcpServers");
    if (!strcmp(agent,"opencode")) {
        json_object *mcp=km_get(doc,"mcp");
        if (mcp && !json_object_is_type(mcp,json_type_object)) goto invalid;
        servers=km_get(mcp,"servers");
        if (mcp) {
            json_object_object_foreach(mcp,key,value) {
                (void)value;
                if (strcmp(key,"servers")) goto invalid;
            }
        }
    }
    if (!servers) return 0;
    if (!json_object_is_type(servers,json_type_object)) goto invalid;
    json_object_object_foreach(servers,id,server) {
        if (!km_valid_id(id) || ad_server_shape(km,agent,server)<0) goto invalid;
    }
    return 0;
invalid:
    return km_error(km,"unsupported native %s schema; existing configuration preserved",agent);
}
