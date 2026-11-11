#include "subagents.h"
#include "adapters.h"
#include "adapters_toml_vendor.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static toml_table_t *parse(Km *km, const char *text) {
    char error[256], *copy=strdup(text);
    if (!copy) { km_error(km,"out of memory"); return NULL; }
    toml_table_t *table=toml_parse(copy,error,sizeof(error)); free(copy);
    if (!table) km_error(km,"invalid Codex subagent TOML (details suppressed to protect native settings)");
    return table;
}

static bool encoded_nul(toml_raw_t raw) {
    if (!raw || raw[0]!='"') return false;
    for (size_t i=1;raw[i];i++) {
        if (raw[i]!='\\' || !raw[i+1]) continue;
        if (!strncmp(raw+i,"\\u0000",6) || !strncmp(raw+i,"\\U00000000",10)) return true;
        i++;
    }
    return false;
}

static int required(Km *km, toml_table_t *table, json_object *doc, const char *field, const char *key) {
    if (encoded_nul(toml_raw_in(table,field))) return km_error(km,"subagent strings cannot contain NUL characters");
    toml_datum_t datum=toml_string_in(table,field);
    if (!datum.ok) return km_error(km,"Codex subagent requires name, description and developer_instructions strings");
    bool valid=*datum.u.s!=0;
    if (valid) json_object_object_add(doc,key,json_object_new_string(datum.u.s));
    free(datum.u.s);
    return valid ? 0 : km_error(km,"Codex subagent metadata and instructions must not be empty");
}

json_object *km_subagent_toml(Km *km, const char *text) {
    toml_table_t *table=parse(km,text);
    if (!table) return NULL;
    json_object *doc=json_object_new_object();
    int result=required(km,table,doc,"name","name");
    if (!result) result=required(km,table,doc,"description","description");
    if (!result) result=required(km,table,doc,"developer_instructions","instructions");
    toml_free(table);
    if (result<0) { json_object_put(doc); return NULL; }
    return doc;
}

typedef struct { char quote; bool multi; unsigned depth; } Lex;

static size_t scan(Lex *lex, const char *text, size_t begin, size_t end) {
    for (size_t p=begin;p<end;p++) {
        char ch=text[p];
        if (lex->quote) {
            if (ch=='\\' && lex->quote=='"') { p++; continue; }
            if (ch!=lex->quote) continue;
            if (!lex->multi) { lex->quote=0; continue; }
            size_t count=1; while (p+count<end && text[p+count]==ch) count++;
            if (count>=3) { lex->quote=0; lex->multi=false; p+=count-1; }
        } else if (ch=='#') return p;
        else if (ch=='"' || ch=='\'') {
            lex->quote=ch; lex->multi=p+2<end && text[p+1]==ch && text[p+2]==ch;
            if (lex->multi) p+=2;
        } else if (ch=='[' || ch=='{') lex->depth++;
        else if ((ch==']' || ch=='}') && lex->depth) lex->depth--;
    }
    return end;
}

static bool key_start(const char *text, size_t start, size_t end, const char *key, size_t *value) {
    size_t p=start, length=strlen(key);
    while (p<end && isspace((unsigned char)text[p])) p++;
    char quote=0;
    if (p<end && (text[p]=='"' || text[p]=='\'')) quote=text[p++];
    if (end-p<length || strncmp(text+p,key,length)) return false;
    p+=length;
    if (quote && (p==end || text[p++]!=quote)) return false;
    while (p<end && isspace((unsigned char)text[p])) p++;
    if (p==end || text[p++]!='=') return false;
    while (p<end && isspace((unsigned char)text[p])) p++;
    *value=p; return p<end;
}

static int value_end(Km *km, const char *text, size_t start, size_t *finish) {
    Lex lex={0};
    for (size_t line=start;text[line];) {
        size_t end=line; while (text[end] && text[end]!='\n') end++;
        size_t content=scan(&lex,text,line,end);
        if (!lex.quote && !lex.depth) {
            while (content>start && isspace((unsigned char)text[content-1])) content--;
            *finish=content; return 0;
        }
        line=text[end] ? end+1 : end;
    }
    return km_error(km,"cannot locate complete Codex subagent field; source preserved");
}

static int field_span(Km *km, const char *text, const char *key, size_t *start, size_t *finish) {
    Lex lex={0};
    for (size_t line=0;text[line];) {
        size_t end=line; while (text[end] && text[end]!='\n') end++;
        size_t first=line; while (first<end && isspace((unsigned char)text[first])) first++;
        if (!lex.quote && !lex.depth) {
            if (text[first]=='[') break;
            if (key_start(text,line,end,key,start)) return value_end(km,text,*start,finish);
        }
        scan(&lex,text,line,end); line=text[end] ? end+1 : end;
    }
    return km_error(km,"unsupported Codex subagent key syntax; source preserved");
}

static int edit_field(Km *km, char **text, const char *key, const char *value) {
    size_t begin=0,end=0;
    if (field_span(km,*text,key,&begin,&end)<0) return -1;
    json_object *string=json_object_new_string(value);
    const char *quoted=json_object_to_json_string_ext(string,JSON_C_TO_STRING_PLAIN|JSON_C_TO_STRING_NOSLASHESCAPE);
    int result=ad_replace(km,text,begin,end,quoted);
    json_object_put(string); return result;
}

char *km_subagent_toml_edit(Km *km, const char *text, const char *id,
                           const char *description, const char *instructions) {
    json_object *doc=km_subagent_toml(km,text);
    if (!doc) return NULL;
    char *result=strdup(text); int status=0;
    if (strcmp(km_string(doc,"name"),id)) status=edit_field(km,&result,"name",id);
    if (!status && description) status=edit_field(km,&result,"description",description);
    if (!status && instructions) status=edit_field(km,&result,"developer_instructions",instructions);
    json_object_put(doc);
    if (status<0) { free(result); return NULL; }
    doc=km_subagent_toml(km,result);
    if (!doc) { free(result); return NULL; }
    json_object_put(doc); return result;
}
