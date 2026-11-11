#include "adapters.h"
#include "adapters_toml_vendor.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static toml_table_t *parse(Km *km, const char *text) {
    char error[256], *copy=strdup(text);
    if (!copy) { km_error(km,"out of memory"); return NULL; }
    toml_table_t *table=toml_parse(copy,error,sizeof(error)); free(copy);
    if (!table) km_error(km,"invalid native TOML configuration (details suppressed to protect credentials)");
    return table;
}

static json_object *table_json(Km *km, const toml_table_t *table);

static json_object *raw_json(Km *km, toml_raw_t raw) {
    char *text=NULL; int boolean; int64_t integer; double decimal;
    if (toml_rtos(raw,&text)==0) { json_object *v=json_object_new_string(text); free(text); return v; }
    if (toml_rtob(raw,&boolean)==0) return json_object_new_boolean(boolean);
    if (toml_rtoi(raw,&integer)==0) return json_object_new_int64(integer);
    if (toml_rtod(raw,&decimal)==0) return json_object_new_double(decimal);
    km_error(km,"unsupported TOML value inside MCP definition"); return NULL;
}

static json_object *array_json(Km *km, const toml_array_t *array) {
    json_object *result=json_object_new_array();
    for (int i=0;i<toml_array_nelem(array);i++) {
        toml_raw_t raw=toml_raw_at(array,i);
        toml_array_t *nested=toml_array_at(array,i);
        toml_table_t *table=toml_table_at(array,i);
        json_object *value=raw ? raw_json(km,raw) : nested ? array_json(km,nested) : table_json(km,table);
        if (!value) { json_object_put(result); return NULL; }
        json_object_array_add(result,value);
    }
    return result;
}

static json_object *table_json(Km *km, const toml_table_t *table) {
    if (!table) { km_error(km,"unsupported TOML MCP table"); return NULL; }
    json_object *result=json_object_new_object();
    for (int i=0;;i++) {
        const char *key=toml_key_in(table,i);
        if (!key) break;
        toml_raw_t raw=toml_raw_in(table,key);
        toml_array_t *array=toml_array_in(table,key);
        json_object *value=raw ? raw_json(km,raw) : array ? array_json(km,array) : table_json(km,toml_table_in(table,key));
        if (!value) { json_object_put(result); return NULL; }
        json_object_object_add(result,key,value);
    }
    return result;
}

json_object *ad_toml_ids(Km *km, const char *text) {
    toml_table_t *root=parse(km,text);
    if (!root) return NULL;
    json_object *ids=json_object_new_array();
    toml_table_t *servers=toml_table_in(root,"mcp_servers");
    for (int i=0;servers;i++) {
        const char *key=toml_key_in(servers,i);
        if (!key) break;
        if (!km_valid_id(key) || !toml_table_in(servers,key)) {
            km_error(km,"unsupported Codex MCP identifier or server value"); json_object_put(ids); ids=NULL; break;
        }
        json_object_array_add(ids,json_object_new_string(key));
    }
    toml_free(root); return ids;
}

json_object *ad_toml_parse(Km *km, const char *raw) {
    toml_table_t *root=parse(km,raw);
    if (!root) return NULL;
    toml_table_t *servers=toml_table_in(root,"mcp_servers");
    const char *id=servers ? toml_key_in(servers,0) : NULL;
    json_object *result=id ? table_json(km,toml_table_in(servers,id)) : NULL;
    if (!result && !km->error[0]) km_error(km,"unsupported Codex MCP table");
    toml_free(root); return result;
}

typedef struct { char quote; bool multi; unsigned depth; } Lex;

static size_t scan_content(Lex *lex, const char *s, size_t begin, size_t end) {
    for (size_t p=begin;p<end;p++) {
        char c=s[p];
        if (lex->quote) {
            if (c=='\\' && lex->quote=='"') { p++; continue; }
            if (c!=lex->quote) continue;
            if (!lex->multi) { lex->quote=0; continue; }
            size_t count=1;
            while (p+count<end && s[p+count]==c) count++;
            if (count>=3) { lex->quote=0; lex->multi=false; p+=count-1; }
        } else if (c=='#') return p;
        else if (c=='"' || c=='\'') {
            lex->quote=c; lex->multi=p+2<end && s[p+1]==c && s[p+2]==c;
            if (lex->multi) p+=2;
        } else if (c=='[' || c=='{') lex->depth++;
        else if ((c==']' || c=='}') && lex->depth) lex->depth--;
    }
    return end;
}

static void scan_line(Lex *lex, const char *s, size_t begin, size_t end) {
    (void)scan_content(lex,s,begin,end);
}

static char *header_id(const char *s, size_t begin, size_t end) {
    char error[128], *line=strndup(s+begin,end-begin);
    toml_table_t *root=toml_parse(line,error,sizeof(error)); free(line);
    if (!root) return NULL;
    toml_table_t *servers=toml_table_in(root,"mcp_servers");
    const char *id=servers ? toml_key_in(servers,0) : NULL;
    char *copy=id ? strdup(id) : NULL;
    toml_free(root); return copy;
}

static bool table_header(const char *s, size_t begin, size_t end, const char *id,
                         const char **tables, unsigned depth, bool exact) {
    char error[128], *line=strndup(s+begin,end-begin);
    if (!line) return false;
    toml_table_t *root=toml_parse(line,error,sizeof(error)); free(line);
    if (!root) return false;
    toml_table_t *servers=toml_table_in(root,"mcp_servers");
    toml_table_t *server=servers ? toml_table_in(servers,id) : NULL;
    for (unsigned i=0;server && i<depth;i++) server=toml_table_in(server,tables[i]);
    bool main=server && (!exact || !toml_key_in(server,0));
    toml_free(root); return main;
}

static int span(Km *km, const char *text, const char *id, size_t *start, size_t *finish) {
    Lex lex={0}; bool found=false, closed=false;
    size_t length=strlen(text);
    for (size_t begin=0;begin<length;) {
        size_t end=begin; while (text[end] && text[end]!='\n') end++;
        size_t first=begin; while (first<end && isspace((unsigned char)text[first])) first++;
        if (!lex.quote && !lex.depth && text[first]=='[') {
            char *owner=header_id(text,first,end);
            bool matching=owner && !strcmp(owner,id); free(owner);
            if (matching && closed) return km_error(km,"noncontiguous Codex MCP tables need manual consolidation");
            if (matching && !found) { *start=begin; found=true; }
            if (found && !matching && !closed) { *finish=begin; closed=true; }
        }
        scan_line(&lex,text,begin,end);
        begin=text[end] ? end+1 : end;
    }
    if (found && !closed) *finish=length;
    return found ? 1 : 0;
}

char *ad_toml_raw(Km *km, const char *text, const char *id) {
    size_t start=0,end=0;
    int result=span(km,text,id,&start,&end);
    return result>0 ? strndup(text+start,end-start) : NULL;
}

int ad_toml_edit(Km *km, char **text, const char *id, const char *value) {
    size_t start=0,end=0;
    int result=span(km,*text,id,&start,&end);
    if (result<0) return -1;
    if (result) return ad_replace(km,text,start,end,value);
    if (!value) return 0;
    size_t length=strlen(*text), n=strlen(value)+3;
    char *append=malloc(n);
    if (!append) return km_error(km,"out of memory");
    snprintf(append,n,"\n%s\n",value);
    result=ad_replace(km,text,length,length,append); free(append); return result;
}

static char *toml_value(Km *km, json_object *value) {
    enum json_type type=json_object_get_type(value);
    if (type==json_type_string || type==json_type_boolean || type==json_type_int || type==json_type_double)
        return strdup(json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE));
    if (type==json_type_array) {
        char *result=strdup("[");
        for (size_t i=0;i<json_object_array_length(value);i++) {
            char *part=toml_value(km,json_object_array_get_idx(value,i));
            if (!part) { free(result); return NULL; }
            size_t n=strlen(result)+strlen(part)+4; char *next=malloc(n);
            if (!next) { free(part); free(result); km_error(km,"out of memory"); return NULL; }
            snprintf(next,n,"%s%s%s",result,i ? ", " : "",part); free(result); free(part); result=next;
        }
        size_t n=strlen(result); char *next=realloc(result,n+2);
        if (!next) { free(result); km_error(km,"out of memory"); return NULL; }
        next[n]=']'; next[n+1]=0; return next;
    }
    if (type==json_type_object) {
        char *result=strdup("{"); bool first=true;
        json_object_object_foreach(value,key,item) {
            json_object *quoted=json_object_new_string(key); char *part=toml_value(km,item);
            const char *name=json_object_to_json_string_ext(quoted,JSON_C_TO_STRING_PLAIN);
            if (!part) { json_object_put(quoted); free(result); return NULL; }
            size_t n=strlen(result)+strlen(name)+strlen(part)+8; char *next=malloc(n);
            if (!next) { json_object_put(quoted); free(part); free(result); km_error(km,"out of memory"); return NULL; }
            snprintf(next,n,"%s%s%s = %s",result,first ? "" : ", ",name,part);
            first=false; json_object_put(quoted); free(part); free(result); result=next;
        }
        size_t n=strlen(result); char *next=realloc(result,n+2);
        if (!next) { free(result); km_error(km,"out of memory"); return NULL; }
        next[n]='}'; next[n+1]=0; return next;
    }
    km_error(km,"unsupported value in Codex MCP configuration"); return NULL;
}

typedef struct {
    bool found;
    bool multiline;
    size_t line_start, line_end, value_start, value_end;
} TomlField;

static bool assignment_start(const char *s, size_t begin, size_t end, const char *key, size_t *value) {
    size_t p=begin, length=strlen(key);
    while (p<end && isspace((unsigned char)s[p])) p++;
    char quote=0;
    if (p<end && (s[p]=='"' || s[p]=='\'')) quote=s[p++];
    if (end-p<length || strncmp(s+p,key,length)) return false;
    p+=length;
    if (quote && (p==end || s[p++]!=quote)) return false;
    if (p<end && !isspace((unsigned char)s[p]) && s[p]!='=') return false;
    while (p<end && isspace((unsigned char)s[p])) p++;
    if (p==end || s[p++]!='=') return false;
    while (p<end && isspace((unsigned char)s[p])) p++;
    if (p==end || s[p]=='#') return false;
    *value=p; return true;
}

static int value_span(Km *km, const char *s, size_t begin, size_t limit,
                      size_t line_start, TomlField *field) {
    Lex lex={0}; size_t line=begin;
    while (line<limit) {
        size_t end=line; while (end<limit && s[end]!='\n') end++;
        size_t content=scan_content(&lex,s,line,end);
        if (!lex.quote && !lex.depth) {
            size_t value_end=content;
            while (value_end>begin && isspace((unsigned char)s[value_end-1])) value_end--;
            if (value_end<=begin) return km_error(km,"unsupported empty adopted TOML value");
            field->found=true; field->line_start=line_start; field->line_end=end;
            field->value_start=begin; field->value_end=value_end;
            field->multiline=line!=begin; return 0;
        }
        line=end<limit ? end+1 : end;
    }
    return km_error(km,"unterminated adopted TOML value; original configuration preserved");
}

static int find_field(Km *km, const char *text, const char *id, const char *key,
                      TomlField *field, size_t *insert, const char **tables, unsigned depth) {
    size_t begin=0,end=0;
    int located=span(km,text,id,&begin,&end);
    if (located<0) return -1;
    if (!located) return km_error(km,"adopted Codex MCP table is missing; original TOML preserved");
    Lex lex={0}; bool main=false, main_seen=false; *insert=end;
    for (size_t line=begin;line<end;) {
        size_t line_end=line; while (line_end<end && text[line_end]!='\n') line_end++;
        size_t first=line; while (first<line_end && isspace((unsigned char)text[first])) first++;
        if (!lex.quote && !lex.depth && first<line_end && text[first]=='[') {
            bool was_main=main;
            main=table_header(text,first,line_end,id,tables,depth,true);
            if (main) main_seen=true;
            else if (was_main && *insert==end) *insert=line;
        } else if (main && !lex.quote && !lex.depth) {
            size_t value=0;
            if (assignment_start(text,line,line_end,key,&value))
                return value_span(km,text,value,end,line,field)<0 ? -1 : 1;
        }
        scan_line(&lex,text,line,line_end);
        line=line_end<end ? line_end+1 : line_end;
    }
    return main_seen ? 0 : km_error(km,"unsupported Codex MCP table layout; original TOML preserved");
}

static int insert_field(Km *km, char **text, size_t offset, const char *key, const char *value) {
    json_object *name=json_object_new_string(key);
    const char *quoted=json_object_to_json_string_ext(name,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    bool newline=offset && (*text)[offset-1]!='\n';
    size_t size=strlen(quoted)+strlen(value)+(newline ? 6u : 5u);
    char *line=malloc(size);
    if (!line) { json_object_put(name); return km_error(km,"out of memory"); }
    snprintf(line,size,"%s%s = %s\n",newline ? "\n" : "",quoted,value);
    json_object_put(name);
    int result=ad_replace(km,text,offset,offset,line); free(line); return result;
}

static bool trailing_comment(const char *text, const TomlField *field) {
    size_t p=field->value_end;
    while (p<field->line_end && isspace((unsigned char)text[p])) p++;
    return p<field->line_end && text[p]=='#';
}

static int update_field(Km *km, char **text, const char *id, const char *key,
                        bool existed, json_object *desired, const char **tables, unsigned depth) {
    TomlField field={0}; size_t insert=0;
    int found=find_field(km,*text,id,key,&field,&insert,tables,depth);
    if (found<0) return -1;
    if (existed != (found>0)) return km_error(km,"unsupported TOML key syntax for adopted %s; original preserved",key);
    if (!desired) {
        if (!found) return 0;
        if (field.multiline) return km_error(km,"cannot safely remove multiline adopted field; original TOML preserved");
        size_t end=field.value_end;
        if (!trailing_comment(*text,&field)) {
            end=field.line_end;
            if ((*text)[end]=='\n') end++;
        }
        return ad_replace(km,text,field.line_start,end,"");
    }
    char *value=toml_value(km,desired);
    if (!value) return -1;
    int result;
    if (found) {
        if (field.multiline) result=km_error(km,"cannot safely update multiline adopted %s; original TOML preserved",key);
        else result=ad_replace(km,text,field.value_start,field.value_end,value);
    } else result=insert_field(km,text,insert,key,value);
    free(value); return result;
}

static int remove_headers(Km *km, char **text, const char *id, const char **tables, unsigned depth) {
    Lex lex={0};
    for (size_t line=0;(*text)[line];) {
        size_t end=line; while ((*text)[end] && (*text)[end]!='\n') end++;
        size_t first=line; while (first<end && isspace((unsigned char)(*text)[first])) first++;
        if (!lex.quote && !lex.depth && (*text)[first]=='[' &&
            table_header(*text,first,end,id,tables,depth,false)) {
            Lex header={0}; size_t comment=scan_content(&header,*text,line,end);
            size_t finish=comment<end ? comment : end+((*text)[end]=='\n');
            if (ad_replace(km,text,line,finish,"")<0) return -1;
            continue;
        }
        scan_line(&lex,*text,line,end); line=(*text)[end] ? end+1 : end;
    }
    return 0;
}

static int patch_fields(Km *km, char **text, const char *id, json_object *previous,
                        json_object *native, const char **tables, unsigned depth);

static int patch_field(Km *km, char **text, const char *id, const char *key,
                       json_object *old, json_object *desired, const char **tables, unsigned depth) {
    TomlField field={0}; size_t insert=0;
    int found=find_field(km,*text,id,key,&field,&insert,tables,depth);
    if (found<0) return -1;
    if (found || !json_object_is_type(old,json_type_object))
        return update_field(km,text,id,key,old!=NULL,desired,tables,depth);
    if (depth>=32 || (desired && !json_object_is_type(desired,json_type_object)))
        return km_error(km,"unsupported nested Codex MCP value; original preserved");
    tables[depth]=key;
    json_object *empty=desired ? NULL : json_object_new_object();
    int result=patch_fields(km,text,id,old,desired ? desired : empty,tables,depth+1);
    json_object_put(empty);
    if (!result && !desired) result=remove_headers(km,text,id,tables,depth+1);
    return result;
}

static int patch_fields(Km *km, char **text, const char *id, json_object *previous,
                        json_object *native, const char **tables, unsigned depth) {
    json_object_object_foreach(previous,key,old) {
        json_object *desired=km_get(native,key);
        if (!json_object_equal(old,desired) && patch_field(km,text,id,key,old,desired,tables,depth)<0) return -1;
    }
    json_object_object_foreach(native,new_key,value) {
        if (!km_get(previous,new_key) && patch_field(km,text,id,new_key,NULL,value,tables,depth)<0) return -1;
    }
    return 0;
}

char *ad_toml_render(Km *km, json_object *native, const char *id, const char *base) {
    json_object *previous=base ? ad_toml_parse(km,base) : NULL;
    if (base && !previous) return NULL;
    if (previous && json_object_equal(previous,native)) { json_object_put(previous); return strdup(base); }
    /* Patch typed values within this owned table and retain native comments. */
    if (previous) {
        char *result=strdup(base);
        const char *tables[33]={NULL};
        if (!result) { json_object_put(previous); km_error(km,"out of memory"); return NULL; }
        if (patch_fields(km,&result,id,previous,native,tables,0)<0) {
            json_object_put(previous); free(result); return NULL;
        }
        json_object *rendered=ad_toml_parse(km,result);
        bool valid=rendered && json_object_equal(rendered,native);
        if (rendered) json_object_put(rendered);
        json_object_put(previous);
        if (!valid) { free(result); if (!km->error[0]) km_error(km,"edited Codex MCP TOML did not match its native definition"); return NULL; }
        return result;
    }
    char header[256]; snprintf(header,sizeof(header),"[mcp_servers.%s]\n",id);
    char *result=strdup(header);
    json_object_object_foreach(native,key,value) {
        char *part=toml_value(km,value);
        if (!part) { free(result); return NULL; }
        size_t n=strlen(result)+strlen(key)+strlen(part)+8; char *next=malloc(n);
        if (!next) { free(part); free(result); km_error(km,"out of memory"); return NULL; }
        snprintf(next,n,"%s%s = %s\n",result,key,part); free(part); free(result); result=next;
    }
    return result;
}
