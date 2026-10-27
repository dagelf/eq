#define _GNU_SOURCE
#include "json_util.h"
#include "library.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pulse/volume.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


static void mark_dirty(struct library *l) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    l->dirty=true; l->save_at=ts.tv_sec+ts.tv_nsec/1e9+.35;
}
static void flush_log(struct library *l);
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
    if (n!=2 && n!=3) return false;
    for (size_t i=0;i<n;i++) if (!json_object_is_type(json_object_array_get_idx(*keep,i),json_type_string)) return false;
    if (n==3) { *artist=json_object_get_string(json_object_array_get_idx(*keep,1)); *title=json_object_get_string(json_object_array_get_idx(*keep,2)); return true; }
    if (n==2) { *artist=json_object_get_string(json_object_array_get_idx(*keep,0)); *title=json_object_get_string(json_object_array_get_idx(*keep,1)); return true; }
    return false;
}
static void add_device(struct json_object *devices,const char *name) {
    if (!valid_name(name) || !strcmp(name,"eqweb.unselected")) return;
    char key[NAME_SIZE]; output_key(name,key);
    json_object_object_add(devices,key,json_object_new_boolean(true));
}
static int device_order(const void *a,const void *b) {
    return strcmp(json_object_get_string(*(struct json_object *const *)a),json_object_get_string(*(struct json_object *const *)b));
}
static struct json_object *saved_devices(struct library *l) {
    if (l->devices) return l->devices;
    struct json_object *unique=json_object_new_object();
    const char *maps[]={"output_eq","output_volumes"};
    for (size_t i=0;i<2;i++) {
        json_object_object_foreach(json_get(l->data,maps[i]),name,value) { (void)value; add_device(unique,name); }
    }
    json_object_object_foreach(json_get(l->data,"songs"),key,value) {
        (void)value; struct json_object *parts=NULL; const char *artist,*title;
        if (track_parts(key,&artist,&title,&parts) && json_object_array_length(parts)==3)
            add_device(unique,json_object_get_string(json_object_array_get_idx(parts,0)));
        if (parts) json_object_put(parts);
    }
    l->devices=json_object_new_array();
    json_object_object_foreach(unique,name,present) { (void)present; json_object_array_add(l->devices,json_object_new_string(name)); }
    json_object_put(unique); json_object_array_sort(l->devices,device_order); return l->devices;
}
static void invalidate_devices(struct library *l) {
    if (l->devices) json_object_put(l->devices);
    l->devices=NULL;
}
static struct json_object *song_for_output(struct library *l,const char *output,const char *artist,const char *title) {
    char device[NAME_SIZE],key[2048]; output_key(output,device);
    struct json_object *songs=json_get(l->data,"songs");
    if (make_key(key,sizeof(key),device,artist,title) && json_get(songs,key)) return json_get(songs,key);
    if (strncmp(device,"bluez_output.",strlen("bluez_output."))) return NULL;
    /* Read pre-canonicalization Bluetooth keys without losing older song EQs. */
    json_object_object_foreach(songs,old,value) {
        struct json_object *parts=NULL; const char *a,*t;
        bool match=false;
        if (track_parts(old,&a,&t,&parts) && json_object_array_length(parts)==3 && !strcmp(a,artist) && !strcmp(t,title)) {
            char saved[NAME_SIZE]; output_key(json_object_get_string(json_object_array_get_idx(parts,0)),saved);
            match=!strcmp(saved,device);
        }
        if (parts) json_object_put(parts);
        if (match) return value;
    }
    return NULL;
}
static int ensure_fields(struct library *l) {
    bool added=false;
    struct json_object *priority=json_get(l->data,"output_priority");
    if (!priority) { json_object_object_add(l->data,"output_priority",json_object_new_array()); added=true; }
    else {
        if (!json_object_is_type(priority,json_type_array)) return -1;
        for (size_t i=0;i<json_object_array_length(priority);i++) {
            struct json_object *name=json_object_array_get_idx(priority,i);
            if (!json_object_is_type(name,json_type_string) || !valid_name(json_object_get_string(name)) ||
                strlen(json_object_get_string(name))!=(size_t)json_object_get_string_len(name)) return -1;
        }
    }
    const char *maps[]={"output_eq","output_volumes"};
    for (size_t i=0;i<2;i++) {
        struct json_object *map=json_get(l->data,maps[i]);
        if (!map) { json_object_object_add(l->data,maps[i],json_object_new_object()); added=true; continue; }
        if (!json_object_is_type(map,json_type_object)) return -1;
        json_object_object_foreach(map,key,value) {
            if (!valid_name(key)) return -1;
            if (i==0) {
                struct settings settings={0}; if (!parse_eq(value,&settings)) return -1;
            } else {
                struct json_object *volume=json_get(value,"volume");
                double n=json_object_get_double(volume);
                if ((!json_object_is_type(volume,json_type_double) && !json_object_is_type(volume,json_type_int)) ||
                    !isfinite(n) || n<0 || n>100.0*PA_VOLUME_MAX/PA_VOLUME_NORM ||
                    !json_object_is_type(json_get(value,"mute"),json_type_boolean)) return -1;
            }
        }
    }
    struct json_object *v=json_get(l->data,"quickbar");
    if (!v) { json_object_object_add(l->data,"quickbar",json_object_new_array()); added=true; }
    else if (!json_object_is_type(v,json_type_array) || json_object_array_length(v)>26) return -1;
    else for (size_t i=0;i<json_object_array_length(v);i++) {
        struct settings s={0}; if (!parse_eq(json_object_array_get_idx(v,i),&s)) return -1;
    }
    v=json_get(l->data,"maintain_load");
    if (!v) { json_object_object_add(l->data,"maintain_load",json_object_new_boolean(1)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    v=json_get(l->data,"output_log");
    if (!v) { json_object_object_add(l->data,"output_log",json_object_new_boolean(1)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    v=json_get(l->data,"ignore_custom");
    if (!v) { json_object_object_add(l->data,"ignore_custom",json_object_new_boolean(false)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    if (json_object_get_boolean(v)) json_object_object_add(l->data,"remember",json_object_new_boolean(false));
    v=json_get(l->data,"fallback");
    if (!v) { json_object_object_add(l->data,"fallback",json_object_new_boolean(false)); added=true; }
    else if (!json_object_is_type(v,json_type_boolean)) return -1;
    v=json_get(l->data,"fallback_device");
    if (!v) { json_object_object_add(l->data,"fallback_device",json_object_new_string("")); added=true; }
    else if (!json_object_is_type(v,json_type_string) ||
        (*json_object_get_string(v) && !valid_name(json_object_get_string(v))) ||
        strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) return -1;
    if (added) mark_dirty(l);
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
        if (!l->data || json_object_get_int(json_get(l->data,"version"))!=1 ||
            !json_object_is_type(json_get(l->data,"presets"),json_type_object) ||
            !json_object_is_type(json_get(l->data,"songs"),json_type_object) ||
            !json_object_is_type(json_get(l->data,"remember"),json_type_boolean) ||
            !json_object_is_type(json_get(l->data,"player"),json_type_string) ||
            ensure_fields(l)) goto invalid;
        const char *groups[]={"presets","songs"};
        for (int i=0;i<2;i++) {
            json_object_object_foreach(json_get(l->data,groups[i]),key,value) {
                (void)key; struct settings s={0}; if (!parse_eq(value,&s)) goto invalid;
            }
        }
        return 0;
    }
    if (errno!=ENOENT) return -1;
    l->data=json_tokener_parse("{\"version\":1,\"presets\":{},\"songs\":{},\"remember\":true,\"player\":\"\",\"quickbar\":[],\"maintain_load\":true,\"output_log\":true}");
    return l->data?ensure_fields(l):-1;
invalid:
    if (l->data) json_object_put(l->data);
    l->data=NULL; errno=EINVAL; return -1;
}
void library_save(struct library *l) {
    flush_log(l);
    if (!l->dirty) return;
    const char *s=json_object_to_json_string_ext(l->data,JSON_C_TO_STRING_PRETTY);
    if (atomic_write(l->path,s,strlen(s))) snprintf(l->error,sizeof(l->error),"Cannot save presets/song EQ: %s",strerror(errno));
    else { l->dirty=false; l->error[0]=0; }
}
void library_flush(struct library *l,double now) {
    if (now<l->retry_at || (l->dirty && now<l->save_at)) return;
    library_save(l);
    if (l->dirty || *l->log_error) l->retry_at=now+2;
}
void library_close(struct library *l) {
    library_save(l);
    if (l->data) json_object_put(l->data);
    if (l->log_queue) json_object_put(l->log_queue);
    if (l->devices) json_object_put(l->devices);
}
static struct json_object *device_eq(struct library *l,const struct settings *current) {
    char key[NAME_SIZE]; output_key(current->output,key);
    return json_get(json_get(l->data,"output_eq"),key);
}
static bool load_eq(struct library *l,struct json_object *eq,struct settings *current,bool loading) {
    struct settings next=*current;
    if (!parse_eq(eq,&next)) return false;
    if (loading && json_object_get_boolean(json_get(l->data,"maintain_load"))) {
        double target=current->maintain?current->loudness_target:average_gain(current)+current->preamp;
        next.maintain=current->maintain;
        next.loudness_target=target;
        next.preamp=fmax(-24,fmin(12,target-average_gain(&next)));
    }
    *current=next; return true;
}
static bool restore_general(struct library *l,struct settings *current,bool loading) {
    struct settings before=*current;
    if (!load_eq(l,device_eq(l,current),current,loading)) return false;
    if (before.preamp!=current->preamp || before.maintain!=current->maintain || before.loudness_target!=current->loudness_target) return true;
    for (int i=0;i<BANDS;i++) if (before.gains[i]!=current->gains[i]) return true;
    return false;
}
struct json_object *library_snapshot(struct library *l,const struct settings *current) {
    struct json_object *o=json_object_new_object();
    json_object_object_add(o,"output_priority",json_object_get(json_get(l->data,"output_priority")));
    json_object_object_add(o,"presets",json_object_get(json_get(l->data,"presets")));
    json_object_object_add(o,"quickbar",json_object_get(json_get(l->data,"quickbar")));
    json_object_object_add(o,"remember",json_object_get(json_get(l->data,"remember")));
    json_object_object_add(o,"ignore_custom",json_object_get(json_get(l->data,"ignore_custom")));
    json_object_object_add(o,"maintain_load",json_object_get(json_get(l->data,"maintain_load")));
    json_object_object_add(o,"output_log",json_object_get(json_get(l->data,"output_log")));
    json_object_object_add(o,"player",json_object_get(json_get(l->data,"player")));
    json_object_object_add(o,"fallback",json_object_get(json_get(l->data,"fallback")));
    json_object_object_add(o,"fallback_device",json_object_get(json_get(l->data,"fallback_device")));
    json_object_object_add(o,"devices",json_object_get(saved_devices(l)));
    json_object_object_add(o,"device_eq",json_object_get(device_eq(l,current)));
    json_object_object_add(o,"song_count",json_object_new_int(json_object_object_length(json_get(l->data,"songs"))));
    json_object_object_add(o,"song_status",json_object_new_string(l->song_status));
    json_object_object_add(o,"error",json_object_new_string(l->error));
    json_object_object_add(o,"log_error",json_object_new_string(l->log_error));
    json_object_object_add(o,"pending",json_object_new_boolean(l->dirty || (l->log_queue && json_object_array_length(l->log_queue)))); return o;
}
static const char *toggle(struct library *l,struct json_object *body,const char *field) {
    struct json_object *v=json_get(body,"enabled"); if (!json_object_is_type(v,json_type_boolean)) return "Expected enabled: true or false";
    json_object_object_add(l->data,field,json_object_get(v)); return NULL;
}
const char *library_action(struct library *l,struct json_object *body,const struct settings *current) {
    const char *action=json_string(body,"action");
    struct json_object *v=json_get(body,"name"),*presets=json_get(l->data,"presets");
    if (!strcmp(action,"save") || !strcmp(action,"delete")) {
        if (!json_object_is_type(v,json_type_string) || !*json_object_get_string(v) ||
            json_object_get_string_len(v)>128 || strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) return "Use a preset name of 1–128 bytes";
        const char *name=json_object_get_string(v);
        if (!strcmp(action,"save")) {
            if (!json_get(presets,name) && json_object_object_length(presets)>=128) return "Preset limit reached (128); delete a preset first";
            struct settings s=*current;
            if (!parse_eq(json_get(body,"eq"),&s)) return "Preset must include valid EQ values";
            json_object_object_add(presets,name,eq_json(&s));
        } else json_object_object_del(presets,name);
    } else if (!strcmp(action,"remember")) {
        const char *error=toggle(l,body,"remember"); if (error) return error;
        if (json_object_get_boolean(json_get(l->data,"remember")))
            json_object_object_add(l->data,"ignore_custom",json_object_new_boolean(false));
        l->general_pending=!json_object_get_boolean(json_get(l->data,"remember"));
        l->song[0]=0; l->song_status[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"ignore_custom")) {
        const char *error=toggle(l,body,"ignore_custom"); if (error) return error;
        if (json_object_get_boolean(json_get(l->data,"ignore_custom"))) {
            json_object_object_add(l->data,"remember",json_object_new_boolean(false));
            l->general_pending=true;
        }
        l->song[0]=0; l->song_status[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"maintain_load")) {
        const char *error=toggle(l,body,"maintain_load"); if (error) return error;
    } else if (!strcmp(action,"output_log")) {
        const char *error=toggle(l,body,"output_log"); if (error) return error;
    } else if (!strcmp(action,"fallback")) {
        const char *error=toggle(l,body,"fallback"); if (error) return error;
        l->song[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"fallback_device")) {
        v=json_get(body,"device");
        if (!json_object_is_type(v,json_type_string) ||
            (*json_object_get_string(v) && !valid_name(json_object_get_string(v))) ||
            strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) return "Invalid fallback device";
        char key[NAME_SIZE]; output_key(json_object_get_string(v),key);
        json_object_object_add(l->data,"fallback_device",json_object_new_string(key));
        l->song[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"device_eq")) {
        struct settings saved=*current;
        if (strcmp(json_string(body,"output"),current->output)) return "Output changed; select the device again";
        if (!parse_eq(json_get(body,"eq"),&saved)) return "Device EQ must include valid EQ values";
        library_remember_output(l,&saved);
    } else if (!strcmp(action,"player")) {
        v=json_get(body,"player");
        if (!json_object_is_type(v,json_type_string) || json_object_get_string_len(v)>255) return "Invalid player name";
        json_object_object_add(l->data,"player",json_object_get(v)); l->song[0]=0; l->baseline_valid=false;
    } else if (!strcmp(action,"quickbar")) {
        struct json_object *bar=json_get(l->data,"quickbar");
        if (json_object_array_length(bar)>=26) return "Quickbar is full (A–Z)";
        struct settings s=*current;
        if (!parse_eq(json_get(body,"eq"),&s)) return "Quickbar item must include valid EQ values";
        json_object_array_add(bar,eq_json(&s));
    } else if (!strcmp(action,"quickbar_clear")) json_object_object_add(l->data,"quickbar",json_object_new_array());
    else return "Unknown library action";
    mark_dirty(l); return NULL;
}
void library_remember(struct library *l,const struct settings *current) {
    l->general_pending=false;
    if (!*l->song || !json_object_get_boolean(json_get(l->data,"remember"))) { library_remember_output(l,current); return; }
    if (strcmp(l->song_output,current->output)) return;
    struct json_object *songs=json_get(l->data,"songs");
    if (!json_get(songs,l->song) && json_object_object_length(songs)>=1000) {
        snprintf(l->song_status,sizeof(l->song_status),"Song memory is full (1,000 songs)"); return;
    }
    json_object_object_add(songs,l->song,eq_json(current)); mark_dirty(l);
    snprintf(l->song_status,sizeof(l->song_status),"Remembering EQ for this song");
}
void library_init_output(struct library *l,const struct settings *current) {
    if (!device_eq(l,current)) library_remember_output(l,current);
}
void library_prefer_output(struct library *l,const char *output) {
    if (!valid_name(output) || !strcmp(output,"eqweb.unselected")) return;
    struct json_object *old=json_get(l->data,"output_priority"),*priority=json_object_new_array();
    json_object_array_add(priority,json_object_new_string(output));
    for (size_t i=0;i<json_object_array_length(old);i++) {
        struct json_object *entry=json_object_array_get_idx(old,i);
        const char *name=json_object_get_string(entry);
        if (strcmp(output,name) && !same_bluetooth_output(output,name))
            json_object_array_add(priority,json_object_get(entry));
    }
    if (json_object_equal(old,priority)) { json_object_put(priority); return; }
    json_object_object_add(l->data,"output_priority",priority); mark_dirty(l);
}
void library_remember_output(struct library *l,const struct settings *current) {
    char key[NAME_SIZE]; output_key(current->output,key);
    struct json_object *outputs=json_get(l->data,"output_eq"),*eq=eq_json(current);
    if (json_object_equal(json_get(outputs,key),eq)) { json_object_put(eq); return; }
    if (!json_get(outputs,key)) invalidate_devices(l);
    json_object_object_add(outputs,key,eq); mark_dirty(l);
}
void library_select_output(struct library *l,struct settings *current,const char *output) {
    if (!strcmp(current->output,output)) return;
    library_init_output(l,current);
    restore_general(l,current,false);
    char key[NAME_SIZE]; output_key(output,key);
    snprintf(current->output,sizeof(current->output),"%s",output);
    struct json_object *saved=json_get(json_get(l->data,"output_eq"),key);
    if (saved) parse_eq(saved,current);
    l->song[0]=0; l->song_output[0]=0; l->baseline_valid=false;
    library_init_output(l,current);
}
void library_volumes(struct library *l,struct json_object *volumes) {
    if (!volumes || json_object_equal(json_get(l->data,"output_volumes"),volumes)) return;
    if (json_object_object_length(volumes)!=json_object_object_length(json_get(l->data,"output_volumes"))) invalidate_devices(l);
    json_object_object_add(l->data,"output_volumes",json_copy(volumes)); mark_dirty(l);
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
    struct json_object *presets=json_get(l->data,"presets");
    if (!json_get(presets,name) && json_object_object_length(presets)>=128) return;
    json_object_object_add(presets,name,eq_json(current)); mark_dirty(l);
}
static void log_song(struct library *l,const struct settings *current,const char *artist,const char *title) {
    if (!json_object_get_boolean(json_get(l->data,"output_log"))) return;
    struct json_object *o=eq_json(current);
    json_object_object_add(o,"artist",json_object_new_string(artist));
    json_object_object_add(o,"title",json_object_new_string(title));
    json_object_object_add(o,"output",json_object_new_string(current->output));
    time_t now=time(NULL); struct tm tm; char ts[32]="";
    if (gmtime_r(&now,&tm)) strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%SZ",&tm);
    json_object_object_add(o,"time",json_object_new_string(ts));
    if (!l->log_queue) l->log_queue=json_object_new_array();
    if (json_object_array_length(l->log_queue)>=1000) {
        snprintf(l->log_error,sizeof(l->log_error),"Song log backlog is full; this entry could not be queued");
        json_object_put(o); return;
    }
    json_object_array_add(l->log_queue,o);
}
static void finish_song(struct library *l,const struct settings *current) {
    if (!*l->song || strcmp(l->song_output,current->output) ||
        json_object_get_boolean(json_get(l->data,"ignore_custom"))) return;
    if (l->baseline_valid && !eq_same(current,&l->baseline)) save_auto_preset(l,current);
    struct json_object *keep=NULL; const char *artist,*title;
    if (track_parts(l->song,&artist,&title,&keep)) log_song(l,current,artist,title);
    if (keep) json_object_put(keep);
}
/* Roll back an incomplete append before retrying it. The editor's process lock
 * ensures that only one eqweb process writes this log. */
static void flush_log(struct library *l) {
    if (!l->log_queue || !json_object_array_length(l->log_queue)) return;
    int fd=open(l->log_path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if (fd<0) goto fail;
    while (json_object_array_length(l->log_queue)) {
        struct json_object *entry=json_object_array_get_idx(l->log_queue,0);
        const char *line=json_object_to_json_string_ext(entry,JSON_C_TO_STRING_PLAIN);
        off_t start=lseek(fd,0,SEEK_END);
        if (start<0) { close(fd); goto fail; }
        size_t len=strlen(line),written=0;
        while (written<len) {
            ssize_t n=write(fd,line+written,len-written);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) break;
            written+=(size_t)n;
        }
        if (written!=len || write(fd,"\n",1)!=1 || fsync(fd)) {
            int error=errno;
            if (ftruncate(fd,start)) {
                snprintf(l->log_error,sizeof(l->log_error),"Song log append and rollback failed: %s",strerror(errno));
                close(fd); return;
            }
            close(fd); errno=error; goto fail;
        }
        json_object_array_del_idx(l->log_queue,0,1);
    }
    close(fd); l->log_error[0]=0; return;
fail:
    snprintf(l->log_error,sizeof(l->log_error),"Cannot write song log: %s",strerror(errno));
}

bool library_follow(struct library *l,struct json_object *desktop,struct settings *current,bool editing) {
    struct json_object *players=json_get(desktop,"players"),*selected=NULL;
    const char *preferred=json_string(l->data,"player");
    for (size_t i=0;i<json_object_array_length(players);i++) {
        struct json_object *p=json_object_array_get_idx(players,i);
        if (*preferred) { if (!strcmp(json_string(p,"id"),preferred)) { selected=p; break; } }
        else if (!selected || (!strcmp(json_string(p,"status"),"Playing") && strcmp(json_string(selected,"status"),"Playing"))) selected=p;
    }
    json_object_object_add(desktop,"selected_player",selected?json_object_get(selected):json_object_new_object());
    if (!editing) return false;
    library_init_output(l,current);
    bool restored=l->general_pending && restore_general(l,current,true);
    l->general_pending=false;
    if (!selected || !*json_string(selected,"title") || !strcmp(json_string(selected,"status"),"Stopped")) {
        finish_song(l,current);
        if (*l->song && json_object_get_boolean(json_get(l->data,"remember"))) restored=restore_general(l,current,true) || restored;
        l->song[0]=0; l->baseline_valid=false; snprintf(l->song_status,sizeof(l->song_status),"Using the device EQ; waiting for a song title"); return restored;
    }
    const char *artist=json_string(selected,"artist"),*title=json_string(selected,"title");
    char keyed[2048],device[NAME_SIZE]; output_key(current->output,device);
    if (!make_key(keyed,sizeof(keyed),device,artist,title)) { l->song[0]=0; l->baseline_valid=false; return restored; }
    if (!strcmp(l->song,keyed)) return restored;
    if (*l->song) {
        struct json_object *keep=NULL; const char *prev_artist,*prev_title;
        if (track_parts(l->song,&prev_artist,&prev_title,&keep) && (strcmp(prev_artist,artist) || strcmp(prev_title,title)))
            finish_song(l,current);
        if (keep) json_object_put(keep);
    }
    snprintf(l->song,sizeof(l->song),"%s",keyed);
    snprintf(l->song_output,sizeof(l->song_output),"%s",current->output);
    if (json_object_get_boolean(json_get(l->data,"remember"))) {
        char legacy[2048]; make_key(legacy,sizeof(legacy),NULL,artist,title);
        struct json_object *songs=json_get(l->data,"songs"),*saved=song_for_output(l,current->output,artist,title);
        if (!saved) saved=json_get(songs,legacy);
        const char *source=NULL;
        if (!saved && json_object_get_boolean(json_get(l->data,"fallback"))) {
            const char *preferred=json_string(l->data,"fallback_device");
            if (*preferred && strcmp(preferred,device)) { saved=song_for_output(l,preferred,artist,title); if (saved) source=preferred; }
            struct json_object *devices=saved_devices(l);
            for (size_t i=0;!saved && i<json_object_array_length(devices);i++) {
                const char *candidate=json_object_get_string(json_object_array_get_idx(devices,i));
                if (!strcmp(candidate,device)) continue;
                saved=song_for_output(l,candidate,artist,title); if (saved) source=candidate;
            }
        }
        if (saved && load_eq(l,saved,current,true)) {
            if (!source && !json_get(songs,keyed)) { json_object_object_add(songs,keyed,json_copy(saved)); mark_dirty(l); }
            if (source) snprintf(l->song_status,sizeof(l->song_status),"Using this song’s EQ from %s",source);
            else snprintf(l->song_status,sizeof(l->song_status),"Restored this song’s EQ");
            restored=true;
        } else {
            restored=restore_general(l,current,true) || restored;
            snprintf(l->song_status,sizeof(l->song_status),"No saved song EQ; using the device EQ");
        }
        l->baseline=*current; l->baseline_valid=true;
    } else { l->baseline_valid=false; snprintf(l->song_status,sizeof(l->song_status),"Song recall off"); }
    return restored;
}
