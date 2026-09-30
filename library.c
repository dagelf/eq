#define _GNU_SOURCE
#include "library.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static struct json_object *get(struct json_object *o,const char *key) {
    struct json_object *v=NULL; json_object_object_get_ex(o,key,&v); return v;
}
static const char *str(struct json_object *o,const char *key) {
    struct json_object *v=get(o,key); return v?json_object_get_string(v):"";
}
static struct json_object *eq_json(const struct settings *s) {
    struct json_object *o=settings_json(s);
    json_object_object_del(o,"version"); json_object_object_del(o,"output"); return o;
}
static bool make_key(char *out,size_t size,const char *a,const char *b,const char *c) {
    struct json_object *key=json_object_new_array();
    if (a) json_object_array_add(key,json_object_new_string(a));
    json_object_array_add(key,json_object_new_string(b));
    json_object_array_add(key,json_object_new_string(c));
    const char *name=json_object_to_json_string_ext(key,JSON_C_TO_STRING_PLAIN);
    bool ok=name && strlen(name)<size;
    if (ok) snprintf(out,size,"%s",name);
    json_object_put(key); return ok;
}
static bool track_parts(const char *key,const char **artist,const char **title,struct json_object **keep) {
    *keep=json_tokener_parse(key); *artist=""; *title="";
    if (!*keep || !json_object_is_type(*keep,json_type_array)) return false;
    size_t n=json_object_array_length(*keep);
    if (n==3) { *artist=json_object_get_string(json_object_array_get_idx(*keep,1)); *title=json_object_get_string(json_object_array_get_idx(*keep,2)); return true; }
    if (n==2) { *artist=json_object_get_string(json_object_array_get_idx(*keep,0)); *title=json_object_get_string(json_object_array_get_idx(*keep,1)); return true; }
    return false;
}
static int ensure_fields(struct library *l) {
    bool added=false;
    struct json_object *v=get(l->data,"quickbar");
    if (!v) { json_object_object_add(l->data,"quickbar",json_object_new_array()); added=true; }
    else if (!json_object_is_type(v,json_type_array) || json_object_array_length(v)>26) return -1;
    else for (size_t i=0;i<json_object_array_length(v);i++) {
        struct settings s={0}; if (!parse_eq(json_object_array_get_idx(v,i),&s)) return -1;
    }
    v=get(l->data,"maintain_load");
    if (!v) { json_object_object_add(l->data,"maintain_load",json_object_new_boolean(1)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    v=get(l->data,"output_log");
    if (!v) { json_object_object_add(l->data,"output_log",json_object_new_boolean(1)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    if (added) l->dirty=true;
    return 0;
}
int library_open(struct library *l,const char *state_path) {
    if (snprintf(l->path,sizeof(l->path),"%s",state_path)>=(int)sizeof(l->path)) return -1;
    char *slash=strrchr(l->path,'/'); if (!slash) return -1;
    if ((size_t)(slash-l->path)+sizeof("/library-v1.json")>sizeof(l->path)) return -1;
    if (snprintf(l->log_path,sizeof(l->log_path),"%.*s/outputlog.txt",(int)(slash-l->path),l->path)>=(int)sizeof(l->log_path)) return -1;
    strcpy(slash,"/library-v1.json");
    if (access(l->path,F_OK)==0) {
        l->data=json_object_from_file(l->path);
        if (!l->data || json_object_get_int(get(l->data,"version"))!=1 ||
            !json_object_is_type(get(l->data,"presets"),json_type_object) ||
            !json_object_is_type(get(l->data,"songs"),json_type_object) ||
            !json_object_is_type(get(l->data,"remember"),json_type_boolean) ||
            !json_object_is_type(get(l->data,"player"),json_type_string) ||
            ensure_fields(l)) goto invalid;
        const char *groups[]={"presets","songs"};
        for (int i=0;i<2;i++) {
            json_object_object_foreach(get(l->data,groups[i]),key,value) {
                (void)key; struct settings s={0}; if (!parse_eq(value,&s)) goto invalid;
            }
        }
        return 0;
    }
    if (errno!=ENOENT) return -1;
    l->data=json_tokener_parse("{\"version\":1,\"presets\":{},\"songs\":{},\"remember\":true,\"player\":\"\",\"quickbar\":[],\"maintain_load\":true,\"output_log\":true}");
    return l->data?0:-1;
invalid:
    if (l->data) json_object_put(l->data);
    l->data=NULL; errno=EINVAL; return -1;
}
void library_save(struct library *l) {
    if (!l->dirty) return;
    const char *s=json_object_to_json_string_ext(l->data,JSON_C_TO_STRING_PRETTY);
    if (atomic_write(l->path,s,strlen(s))) snprintf(l->error,sizeof(l->error),"Cannot save presets/song EQ: %s",strerror(errno));
    else { l->dirty=false; l->error[0]=0; }
}
void library_close(struct library *l) { library_save(l); if (l->data) json_object_put(l->data); }
struct json_object *library_snapshot(struct library *l) {
    struct json_object *o=json_object_new_object();
    json_object_object_add(o,"presets",json_object_get(get(l->data,"presets")));
    json_object_object_add(o,"quickbar",json_object_get(get(l->data,"quickbar")));
    json_object_object_add(o,"remember",json_object_get(get(l->data,"remember")));
    json_object_object_add(o,"maintain_load",json_object_get(get(l->data,"maintain_load")));
    json_object_object_add(o,"output_log",json_object_get(get(l->data,"output_log")));
    json_object_object_add(o,"player",json_object_get(get(l->data,"player")));
    json_object_object_add(o,"song_count",json_object_new_int(json_object_object_length(get(l->data,"songs"))));
    json_object_object_add(o,"song_status",json_object_new_string(l->song_status));
    json_object_object_add(o,"error",json_object_new_string(l->error));
    json_object_object_add(o,"pending",json_object_new_boolean(l->dirty)); return o;
}
static const char *toggle(struct library *l,struct json_object *body,const char *field) {
    struct json_object *v=get(body,"enabled"); if (!json_object_is_type(v,json_type_boolean)) return "Expected enabled: true or false";
    json_object_object_add(l->data,field,json_object_get(v)); return NULL;
}
const char *library_action(struct library *l,struct json_object *body,const struct settings *current) {
    const char *action=str(body,"action");
    struct json_object *v=get(body,"name"),*presets=get(l->data,"presets");
    if (!strcmp(action,"save") || !strcmp(action,"delete")) {
        if (!json_object_is_type(v,json_type_string) || !*json_object_get_string(v) ||
            json_object_get_string_len(v)>128 || strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) return "Use a preset name of 1–128 bytes";
        const char *name=json_object_get_string(v);
        if (!strcmp(action,"save")) {
            if (!get(presets,name) && json_object_object_length(presets)>=128) return "Preset limit reached (128); delete a preset first";
            struct settings s=*current;
            if (!parse_eq(get(body,"eq"),&s)) return "Preset must include valid EQ values";
            json_object_object_add(presets,name,eq_json(&s));
        } else json_object_object_del(presets,name);
    } else if (!strcmp(action,"remember")) {
        const char *error=toggle(l,body,"remember"); if (error) return error;
        l->song[0]=0; l->song_status[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"maintain_load")) {
        const char *error=toggle(l,body,"maintain_load"); if (error) return error;
    } else if (!strcmp(action,"output_log")) {
        const char *error=toggle(l,body,"output_log"); if (error) return error;
    } else if (!strcmp(action,"player")) {
        v=get(body,"player");
        if (!json_object_is_type(v,json_type_string) || json_object_get_string_len(v)>255) return "Invalid player name";
        json_object_object_add(l->data,"player",json_object_get(v)); l->song[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"quickbar")) {
        struct json_object *bar=get(l->data,"quickbar");
        if (json_object_array_length(bar)>=26) return "Quickbar is full (A–Z)";
        struct settings s=*current;
        if (!parse_eq(get(body,"eq"),&s)) return "Quickbar item must include valid EQ values";
        json_object_array_add(bar,eq_json(&s));
    } else if (!strcmp(action,"quickbar_clear")) json_object_object_add(l->data,"quickbar",json_object_new_array());
    else return "Unknown library action";
    l->dirty=true; return NULL;
}
void library_remember(struct library *l,const struct settings *current) {
    if (!*l->song || !json_object_get_boolean(get(l->data,"remember"))) return;
    struct json_object *songs=get(l->data,"songs");
    if (!get(songs,l->song) && json_object_object_length(songs)>=1000) {
        snprintf(l->song_status,sizeof(l->song_status),"Song memory is full (1,000 songs)"); return;
    }
    json_object_object_add(songs,l->song,eq_json(current)); l->dirty=true;
    snprintf(l->song_status,sizeof(l->song_status),"Remembering EQ for this song");
}
static void save_auto_preset(struct library *l,const struct settings *current) {
    struct json_object *keep=NULL; const char *artist,*title;
    if (!track_parts(l->song,&artist,&title,&keep) || !*title) { if (keep) json_object_put(keep); return; }
    char name[512];
    if (*artist) snprintf(name,sizeof(name),"%s - %s (Auto)",artist,title);
    else snprintf(name,sizeof(name),"%s (Auto)",title);
    if (strlen(name)>128) {
        char clipped[129];
        snprintf(clipped,128-(int)strlen(" (Auto)")+1,"%s",title);
        snprintf(name,sizeof(name),"%s (Auto)",clipped);
    }
    json_object_put(keep);
    struct json_object *presets=get(l->data,"presets");
    if (!get(presets,name) && json_object_object_length(presets)>=128) return;
    json_object_object_add(presets,name,eq_json(current)); l->dirty=true;
}
static void log_song(struct library *l,const struct settings *current,const char *artist,const char *title) {
    if (!json_object_get_boolean(get(l->data,"output_log"))) return;
    struct json_object *o=eq_json(current);
    json_object_object_add(o,"artist",json_object_new_string(artist));
    json_object_object_add(o,"title",json_object_new_string(title));
    json_object_object_add(o,"output",json_object_new_string(current->output));
    time_t now=time(NULL); struct tm tm; char ts[32]="";
    if (gmtime_r(&now,&tm)) strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%SZ",&tm);
    json_object_object_add(o,"time",json_object_new_string(ts));
    const char *line=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    char *parent=strdup(l->log_path); char *slash=parent?strrchr(parent,'/'):NULL;
    if (slash && slash!=parent) { *slash=0; mkdir(parent,0700); }
    free(parent);
    FILE *f=fopen(l->log_path,"a");
    if (!f || fprintf(f,"%s\n",line)<0 || fflush(f) || fsync(fileno(f))) {
        snprintf(l->error,sizeof(l->error),"Cannot write song log: %s",strerror(errno));
        if (f) fclose(f);
    } else fclose(f);
    json_object_put(o);
}
bool library_follow(struct library *l,struct json_object *desktop,struct settings *current,bool editing) {
    struct json_object *players=get(desktop,"players"),*selected=NULL;
    const char *preferred=str(l->data,"player");
    for (size_t i=0;i<json_object_array_length(players);i++) {
        struct json_object *p=json_object_array_get_idx(players,i);
        if (*preferred) { if (!strcmp(str(p,"id"),preferred)) { selected=p; break; } }
        else if (!selected || (!strcmp(str(p,"status"),"Playing") && strcmp(str(selected,"status"),"Playing"))) selected=p;
    }
    json_object_object_add(desktop,"selected_player",selected?json_object_get(selected):json_object_new_object());
    if (!editing) return false;
    if (!selected || !*str(selected,"title") || !strcmp(str(selected,"status"),"Stopped")) {
        l->song[0]=0; l->baseline_valid=false; snprintf(l->song_status,sizeof(l->song_status),"Waiting for a song title"); return false;
    }
    const char *artist=str(selected,"artist"),*title=str(selected,"title");
    char keyed[2048];
    if (!make_key(keyed,sizeof(keyed),current->output,artist,title)) { l->song[0]=0; return false; }
    if (!strcmp(l->song,keyed)) return false;
    if (*l->song && l->baseline_valid && !eq_same(current,&l->baseline)) {
        struct json_object *keep=NULL; const char *prev_artist,*prev_title;
        if (track_parts(l->song,&prev_artist,&prev_title,&keep) && (strcmp(prev_artist,artist) || strcmp(prev_title,title)))
            save_auto_preset(l,current);
        if (keep) json_object_put(keep);
    }
    snprintf(l->song,sizeof(l->song),"%s",keyed);
    bool restored=false;
    if (json_object_get_boolean(get(l->data,"remember"))) {
        char legacy[2048]; make_key(legacy,sizeof(legacy),NULL,artist,title);
        struct json_object *songs=get(l->data,"songs"),*saved=get(songs,keyed);
        if (!saved) saved=get(songs,legacy);
        if (saved && parse_eq(saved,current)) {
            if (!get(songs,keyed)) { json_object_object_add(songs,keyed,eq_json(current)); l->dirty=true; }
            snprintf(l->song_status,sizeof(l->song_status),"Restored this song’s EQ"); restored=true;
        } else library_remember(l,current);
        l->baseline=*current; l->baseline_valid=true;
    } else { l->baseline_valid=false; snprintf(l->song_status,sizeof(l->song_status),"Song recall off"); }
    log_song(l,current,artist,title); return restored;
}
