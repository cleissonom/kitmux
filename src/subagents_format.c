#include "subagents.h"
#include "adapters.h"
#include <stdlib.h>
#include <string.h>

static const char *client_for(const char *format) {
    if (!format) return NULL;
    if (!strcmp(format,"codex") || !strcmp(format,"claude") || !strcmp(format,"opencode")) return format;
    return !strcmp(format,"opencode-json") ? "opencode" : NULL;
}

static bool string_value(json_object *value, bool required) {
    return json_object_is_type(value,json_type_string) &&
        strlen(json_object_get_string(value))==(size_t)json_object_get_string_len(value) &&
        (!required || *json_object_get_string(value));
}

static bool printable_description(const char *text) {
    for (const unsigned char *p=(const unsigned char *)text; *p; p++)
        if ((*p<32 && *p!='\n' && *p!='\r' && *p!='\t') || *p==127) return false;
    return true;
}

static json_object *parse_json(Km *km, const char *text) {
    json_object *doc=ad_parse(km,text);
    if (!doc || !json_object_is_type(doc,json_type_object)) {
        json_object_put(doc); km_error(km,"invalid OpenCode subagent JSON definition"); return NULL;
    }
    const char *mode=km_string(doc,"mode");
    if (!string_value(km_get(doc,"mode"),true) || strcmp(mode,"subagent")) {
        json_object_put(doc); km_error(km,"OpenCode custom profile must use mode: subagent"); return NULL;
    }
    json_object *prompt=km_get(doc,"prompt");
    if (!string_value(km_get(doc,"description"),true) || !string_value(prompt,true)) {
        json_object_put(doc); km_error(km,"subagent description and instructions must be non-empty strings"); return NULL;
    }
    json_object *disabled=km_get(doc,"disabled");
    if (disabled && !json_object_is_type(disabled,json_type_boolean)) {
        json_object_put(doc); km_error(km,"OpenCode subagent disabled must be a boolean"); return NULL;
    }
    json_object_object_add(doc,"instructions",json_object_get(prompt));
    return doc;
}

static json_object *parse_document(Km *km, const char *text, const char *format) {
    if (!strcmp(format,"codex")) return km_subagent_toml(km,text);
    if (!strcmp(format,"opencode-json")) return parse_json(km,text);
    return km_subagent_markdown(km,text,format);
}

static json_object *snapshot(json_object *doc, const char *text, const char *format) {
    json_object *result=json_object_new_object();
    json_object_object_add(result,"text",json_object_new_string(text));
    json_object_object_add(result,"format",json_object_new_string(format));
    const char *fields[]={"description","instructions"};
    for (size_t i=0;i<2;i++) json_object_object_add(result,fields[i],json_object_get(km_get(doc,fields[i])));
    json_object_object_add(result,"disabled",json_object_new_boolean(json_object_get_boolean(km_get(doc,"disabled"))));
    if (km_get(doc,"requires")) json_object_object_add(result,"requires",json_object_get(km_get(doc,"requires")));
    if (km_get(doc,"requires_review")) json_object_object_add(result,"requires_review",json_object_get(km_get(doc,"requires_review")));
    return result;
}

json_object *km_subagent_parse(Km *km, const char *text, const char *format, const char *id) {
    const char *client=client_for(format);
    if (!client || !km_valid_id(id) || !text || strlen(text)>KM_MAX_FILE) {
        km_error(km,"expected subagent format codex, claude, opencode or opencode-json and a valid ID"); return NULL;
    }
    json_object *doc=parse_document(km,text,format);
    if (!doc) return NULL;
    const char *name=km_string(doc,"name");
    if (name && strcmp(name,id)) {
        json_object_put(doc); km_error(km,"native subagent name must match its managed ID; preserve or rename the source definition"); return NULL;
    }
    json_object *resource=json_object_new_object(), *native=json_object_new_object();
    json_object_object_add(resource,"description",json_object_get(km_get(doc,"description")));
    json_object_object_add(resource,"instructions",json_object_get(km_get(doc,"instructions")));
    json_object_object_add(resource,"portable",json_object_new_boolean(false));
    json_object_object_add(resource,"enabled",json_object_new_boolean(true));
    json_object_object_add(resource,"agents",json_object_new_object());
    json_object_object_add(native,client,snapshot(doc,text,format));
    json_object_object_add(resource,"native",native);
    json_object_put(doc);
    return resource;
}

bool km_subagent_compatible(json_object *resource, const char *client, char *reason, size_t size) {
    bool compatible=km_valid_agent(client) &&
        (json_object_get_boolean(km_get(resource,"portable")) || km_get(km_get(resource,"native"),client));
    if (reason && size) snprintf(reason,size,"%s",compatible ? "Native profile available" : "Requires an explicit native variant for this client");
    return compatible;
}

static int validate_requirements(Km *km, json_object *variant) {
    json_object *requires=km_get(variant,"requires");
    if (!requires) return 0;
    if (!json_object_is_type(requires,json_type_array)) return km_error(km,"invalid subagent dependencies");
    for (size_t i=0;i<json_object_array_length(requires);i++) {
        json_object *value=json_object_array_get_idx(requires,i);
        if (!string_value(value,true)) return km_error(km,"invalid subagent dependency reference");
        const char *ref=json_object_get_string(value), *id=NULL;
        if (!strncmp(ref,"skill:",6)) id=ref+6;
        else if (!strncmp(ref,"mcp:",4)) id=ref+4;
        if (!id || !km_valid_id(id)) return km_error(km,"invalid subagent dependency reference");
    }
    return 0;
}

static int validate_variant(Km *km, const char *client, json_object *variant) {
    const char *format=km_string(variant,"format"), *owner=client_for(format);
    if (!json_object_is_type(variant,json_type_object) || !string_value(km_get(variant,"format"),true) || !owner || strcmp(owner,client) ||
        !string_value(km_get(variant,"text"),true) ||
        !string_value(km_get(variant,"description"),true) || !string_value(km_get(variant,"instructions"),true))
        return km_error(km,"invalid native subagent snapshot");
    const char *flags[]={"disabled","requires_review"};
    for (size_t i=0;i<2;i++) {
        json_object *flag=km_get(variant,flags[i]);
        if (flag && !json_object_is_type(flag,json_type_boolean)) return km_error(km,"invalid subagent native flag");
    }
    json_object *doc=parse_document(km,km_string(variant,"text"),format);
    if (!doc) return -1;
    json_object_put(doc); return validate_requirements(km,variant);
}

int km_subagent_validate(Km *km, const char *id, json_object *resource) {
    if (!km_valid_id(id) || !json_object_is_type(resource,json_type_object) ||
        !string_value(km_get(resource,"description"),true) || !string_value(km_get(resource,"instructions"),true) ||
        !printable_description(km_string(resource,"description"))) return km_error(km,"invalid subagent description or instructions");
    const char *serialized=json_object_to_json_string_ext(resource,JSON_C_TO_STRING_PLAIN);
    json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT|JSON_TOKENER_VALIDATE_UTF8);
    json_object *checked=json_tokener_parse_ex(tok,serialized,(int)strlen(serialized)+1);
    bool utf8=json_tokener_get_error(tok)==json_tokener_success;
    json_object_put(checked); json_tokener_free(tok);
    if (!utf8) return km_error(km,"subagent text must be valid UTF-8");
    json_object *portable=km_get(resource,"portable"), *native=km_get(resource,"native");
    if (!json_object_is_type(portable,json_type_boolean) ||
        (native && !json_object_is_type(native,json_type_object))) return km_error(km,"invalid subagent compatibility metadata");
    if (!json_object_get_boolean(portable) && (!native || !json_object_object_length(native)))
        return km_error(km,"native subagent needs at least one client variant");
    if (native) {
        json_object_object_foreach(native,client,variant) {
            if (!km_valid_agent(client) || validate_variant(km,client,variant)<0) return km_error(km,"invalid native subagent variant");
        }
    }
    return 0;
}

static char *quote(const char *value) {
    json_object *string=json_object_new_string(value);
    char *result=strdup(json_object_to_json_string_ext(string,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE));
    json_object_put(string); return result;
}

static char *portable_text(Km *km, json_object *resource, const char *id, const char *client) {
    char *description=quote(km_string(resource,"description")), *instructions=quote(km_string(resource,"instructions"));
    if (!description || !instructions) { free(description); free(instructions); km_error(km,"out of memory"); return NULL; }
    const char *body=km_string(resource,"instructions");
    size_t length=strlen(description)+strlen(instructions)+strlen(body)+strlen(id)+160;
    char *text=malloc(length);
    if (text && !strcmp(client,"codex"))
        snprintf(text,length,"name = \"%s\"\ndescription = %s\ndeveloper_instructions = %s\n",id,description,instructions);
    else if (text && !strcmp(client,"claude"))
        snprintf(text,length,"---\nname: \"%s\"\ndescription: %s\n---\n%s",id,description,body);
    else if (text) snprintf(text,length,"---\ndescription: %s\nmode: subagent\n---\n%s",description,body);
    free(description); free(instructions);
    if (!text) km_error(km,"out of memory");
    return text;
}

static char *json_edit(Km *km, const char *text, const char *description, const char *instructions, bool disabled) {
    char *result=strdup(text);
    const char *values[]={description,instructions}, *keys[]={"description","prompt"};
    for (size_t i=0;result && i<2;i++) {
        if (!values[i]) continue;
        char *quoted=quote(values[i]);
        int status=ad_json_edit(km,&result,keys[i],quoted); free(quoted);
        if (status<0) { free(result); return NULL; }
    }
    if (result && disabled && ad_json_edit(km,&result,"disabled","false")<0) { free(result); return NULL; }
    return result;
}

char *km_subagent_render(Km *km, json_object *resource, const char *id, const char *client) {
    if (km_subagent_validate(km,id,resource)<0) return NULL;
    if (!km_subagent_compatible(resource,client,NULL,0)) {
        km_error(km,"subagent requires an explicit native variant for this client"); return NULL;
    }
    json_object *native=km_get(km_get(resource,"native"),client);
    if (!native) return portable_text(km,resource,id,client);
    const char *description=km_string(resource,"description"), *instructions=km_string(resource,"instructions");
    if (!strcmp(description,km_string(native,"description"))) description=NULL;
    if (!strcmp(instructions,km_string(native,"instructions"))) instructions=NULL;
    const char *text=km_string(native,"text"), *format=km_string(native,"format");
    if (!strcmp(format,"codex")) return km_subagent_toml_edit(km,text,id,description,instructions);
    if (!strcmp(format,"opencode-json")) return json_edit(km,text,description,instructions,json_object_get_boolean(km_get(native,"disabled")));
    return km_subagent_markdown_edit(km,text,client,id,description,instructions);
}
