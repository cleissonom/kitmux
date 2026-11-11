#include "subagents.h"
#include "adapters.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

typedef struct {
    yaml_parser_t parser;
    yaml_document_t document;
    size_t start, body;
    bool initialized, loaded;
} Frontmatter;

static void close_frontmatter(Frontmatter *front) {
    if (front->loaded) yaml_document_delete(&front->document);
    if (front->initialized) yaml_parser_delete(&front->parser);
}

static bool delimiter(const char *text, size_t begin, size_t *next) {
    size_t end=begin; while (text[end] && text[end]!='\n') end++;
    size_t trimmed=end;
    if (trimmed>begin && text[trimmed-1]=='\r') trimmed--;
    if (trimmed-begin!=3 || strncmp(text+begin,"---",3)) return false;
    *next=text[end] ? end+1 : end; return true;
}

static int open_frontmatter(Km *km, const char *text, Frontmatter *front) {
    memset(front,0,sizeof(*front));
    if (!delimiter(text,0,&front->start)) return km_error(km,"native Markdown subagent requires YAML frontmatter");
    size_t end=front->start, next=0;
    while (text[end] && !delimiter(text,end,&next)) {
        while (text[end] && text[end]!='\n') end++;
        if (text[end]) end++;
    }
    if (!text[end] || !next) return km_error(km,"unterminated subagent frontmatter");
    front->body=next;
    front->initialized=yaml_parser_initialize(&front->parser);
    if (!front->initialized) return km_error(km,"out of memory");
    yaml_parser_set_input_string(&front->parser,(const unsigned char *)text+front->start,end-front->start);
    front->loaded=yaml_parser_load(&front->parser,&front->document);
    yaml_node_t *root=front->loaded ? yaml_document_get_root_node(&front->document) : NULL;
    if (!root || root->type!=YAML_MAPPING_NODE) return km_error(km,"invalid native subagent frontmatter");
    return 0;
}

static bool scalar(yaml_node_t *node) {
    return node && node->type==YAML_SCALAR_NODE &&
        !memchr(node->data.scalar.value,0,node->data.scalar.length);
}

static yaml_node_t *field(Frontmatter *front, const char *name) {
    yaml_node_t *root=yaml_document_get_root_node(&front->document);
    for (yaml_node_pair_t *pair=root->data.mapping.pairs.start;pair<root->data.mapping.pairs.top;pair++) {
        yaml_node_t *key=yaml_document_get_node(&front->document,pair->key);
        if (scalar(key) && !strcmp((const char *)key->data.scalar.value,name))
            return yaml_document_get_node(&front->document,pair->value);
    }
    return NULL;
}

static int unique_keys(Km *km, Frontmatter *front) {
    yaml_node_t *root=yaml_document_get_root_node(&front->document);
    for (yaml_node_pair_t *pair=root->data.mapping.pairs.start;pair<root->data.mapping.pairs.top;pair++) {
        yaml_node_t *key=yaml_document_get_node(&front->document,pair->key);
        if (!scalar(key)) return km_error(km,"subagent frontmatter keys must be scalar strings");
        for (yaml_node_pair_t *previous=root->data.mapping.pairs.start;previous<pair;previous++) {
            yaml_node_t *other=yaml_document_get_node(&front->document,previous->key);
            if (scalar(other) && !strcmp((const char *)key->data.scalar.value,(const char *)other->data.scalar.value))
                return km_error(km,"duplicate subagent frontmatter key");
        }
    }
    return 0;
}

char *km_subagent_markdown_mode(Km *km, const char *text) {
    Frontmatter front;
    if (open_frontmatter(km,text,&front)<0) { close_frontmatter(&front); return NULL; }
    yaml_node_t *node=field(&front,"mode"); char *result=NULL;
    if (unique_keys(km,&front)<0 || (node && !scalar(node))) km_error(km,"invalid native subagent frontmatter");
    else result=strdup(node ? (const char *)node->data.scalar.value : "");
    close_frontmatter(&front); return result;
}

static bool looks_typed(yaml_node_t *node) {
    if (!scalar(node) || node->data.scalar.style!=YAML_PLAIN_SCALAR_STYLE) return false;
    const char *text=(const char *)node->data.scalar.value;
    if (!strcmp(text,"true") || !strcmp(text,"false") || !strcmp(text,"null") || !strcmp(text,"~")) return true;
    char *end; (void)strtod(text,&end);
    return end!=text && !*end;
}

static int required_string(Km *km, Frontmatter *front, json_object *doc, const char *key) {
    yaml_node_t *node=field(front,key);
    if (!scalar(node) || !node->data.scalar.length || looks_typed(node))
        return km_error(km,"native subagent requires non-empty string metadata");
    json_object_object_add(doc,key,json_object_new_string_len((const char *)node->data.scalar.value,(int)node->data.scalar.length));
    return 0;
}

/* libyaml marks count Unicode characters; adapters edit UTF-8 byte spans. */
static size_t mark_offset(const char *text, size_t start, size_t index) {
    size_t offset=start;
    while (index-- && text[offset]) {
        offset++;
        while (((unsigned char)text[offset]&0xc0)==0x80) offset++;
    }
    return offset;
}

static int scalar_span(Km *km, Frontmatter *front, const char *text, const char *key,
                       yaml_node_t *node, size_t *begin, size_t *end) {
    *begin=mark_offset(text,front->start,node->start_mark.index);
    *end=mark_offset(text,front->start,node->end_mark.index);
    yaml_node_t *root=yaml_document_get_root_node(&front->document);
    for (yaml_node_pair_t *pair=root->data.mapping.pairs.start;pair<root->data.mapping.pairs.top;pair++) {
        yaml_node_t *name=yaml_document_get_node(&front->document,pair->key);
        if (!scalar(name) || strcmp((const char *)name->data.scalar.value,key) || node->start_mark.index>=name->end_mark.index) continue;
        size_t p=mark_offset(text,front->start,name->end_mark.index);
        while (text[p]==' ' || text[p]=='\t') p++;
        if (text[p++]!=':') return km_error(km,"cannot safely edit aliased subagent metadata");
        while (text[p]==' ' || text[p]=='\t') p++;
        if (text[p]!='*') return km_error(km,"cannot safely edit aliased subagent metadata");
        *begin=p++;
        while (text[p] && !isspace((unsigned char)text[p]) && text[p]!='#') p++;
        *end=p; return 0;
    }
    return *begin<*end ? 0 : km_error(km,"cannot safely edit native subagent metadata");
}

static void requirements(Frontmatter *front, json_object *doc) {
    const char *keys[]={"skills","mcpServers"}, *prefixes[]={"skill:","mcp:"};
    json_object *refs=json_object_new_array(); bool review=false;
    for (size_t i=0;i<2;i++) {
        yaml_node_t *list=field(front,keys[i]);
        if (!list) continue;
        if (list->type!=YAML_SEQUENCE_NODE) { review=true; continue; }
        for (yaml_node_item_t *item=list->data.sequence.items.start;item<list->data.sequence.items.top;item++) {
            yaml_node_t *value=yaml_document_get_node(&front->document,*item);
            if (!scalar(value) || !km_valid_id((const char *)value->data.scalar.value)) { review=true; continue; }
            char reference[80]; snprintf(reference,sizeof(reference),"%s%s",prefixes[i],value->data.scalar.value);
            json_object_array_add(refs,json_object_new_string(reference));
        }
    }
    if (json_object_array_length(refs)) json_object_object_add(doc,"requires",refs);
    else json_object_put(refs);
    if (review) json_object_object_add(doc,"requires_review",json_object_new_boolean(true));
}

static int native_flags(Km *km, Frontmatter *front, json_object *doc, const char *client) {
    if (!strcmp(client,"claude")) { requirements(front,doc); return 0; }
    yaml_node_t *mode=field(front,"mode"), *disabled=field(front,"disabled");
    if (!scalar(mode) || strcmp((const char *)mode->data.scalar.value,"subagent"))
        return km_error(km,"OpenCode custom profile must use mode: subagent");
    if (disabled && (!scalar(disabled) || disabled->data.scalar.style!=YAML_PLAIN_SCALAR_STYLE ||
        (strcmp((const char *)disabled->data.scalar.value,"true") && strcmp((const char *)disabled->data.scalar.value,"false"))))
        return km_error(km,"OpenCode subagent disabled must be a boolean");
    json_object_object_add(doc,"disabled",json_object_new_boolean(disabled && !strcmp((const char *)disabled->data.scalar.value,"true")));
    return 0;
}

json_object *km_subagent_markdown(Km *km, const char *text, const char *client) {
    Frontmatter front;
    if (open_frontmatter(km,text,&front)<0) { close_frontmatter(&front); return NULL; }
    json_object *doc=json_object_new_object();
    int result=unique_keys(km,&front);
    if (!result) result=required_string(km,&front,doc,"description");
    if (!result && !strcmp(client,"claude")) result=required_string(km,&front,doc,"name");
    if (!result && !text[front.body]) result=km_error(km,"subagent instructions must not be empty");
    if (!result) result=native_flags(km,&front,doc,client);
    if (!result) json_object_object_add(doc,"instructions",json_object_new_string(text+front.body));
    close_frontmatter(&front);
    if (result<0) { json_object_put(doc); return NULL; }
    return doc;
}

static int replace_scalar(Km *km, char **text, const char *key, const char *value) {
    Frontmatter front;
    if (open_frontmatter(km,*text,&front)<0) { close_frontmatter(&front); return -1; }
    yaml_node_t *node=field(&front,key);
    if (!scalar(node)) { close_frontmatter(&front); return km_error(km,"cannot safely edit native subagent metadata"); }
    size_t begin=0,end=0;
    if (scalar_span(km,&front,*text,key,node,&begin,&end)<0) { close_frontmatter(&front); return -1; }
    json_object *string=json_object_new_string(value);
    const char *quoted=json_object_to_json_string_ext(string,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    bool newline=end>begin && (*text)[end-1]=='\n';
    size_t length=strlen(quoted)+2; char *replacement=malloc(length);
    if (replacement) snprintf(replacement,length,"%s%s",quoted,newline ? "\n" : "");
    close_frontmatter(&front); json_object_put(string);
    if (!replacement) return km_error(km,"out of memory");
    int result=ad_replace(km,text,begin,end,replacement); free(replacement); return result;
}

static int enable_native(Km *km, char **text) {
    Frontmatter front;
    if (open_frontmatter(km,*text,&front)<0) { close_frontmatter(&front); return -1; }
    yaml_node_t *node=field(&front,"disabled");
    bool disabled=scalar(node) && !strcmp((const char *)node->data.scalar.value,"true");
    size_t begin=0,end=0;
    if (disabled && scalar_span(km,&front,*text,"disabled",node,&begin,&end)<0) { close_frontmatter(&front); return -1; }
    close_frontmatter(&front);
    return disabled ? ad_replace(km,text,begin,end,"false") : 0;
}

char *km_subagent_markdown_edit(Km *km, const char *text, const char *client, const char *id,
                               const char *description, const char *instructions) {
    json_object *doc=km_subagent_markdown(km,text,client);
    if (!doc) return NULL;
    char *result=strdup(text); int status=0;
    const char *name=km_string(doc,"name");
    if (name && strcmp(name,id)) status=replace_scalar(km,&result,"name",id);
    if (!status && description) status=replace_scalar(km,&result,"description",description);
    if (!status && instructions) {
        Frontmatter front;
        status=open_frontmatter(km,result,&front);
        if (!status) status=ad_replace(km,&result,front.body,strlen(result),instructions);
        close_frontmatter(&front);
    }
    if (!status && !strcmp(client,"opencode")) status=enable_native(km,&result);
    json_object_put(doc);
    if (status<0) { free(result); return NULL; }
    doc=km_subagent_markdown(km,result,client);
    if (!doc) { free(result); return NULL; }
    json_object_put(doc); return result;
}
