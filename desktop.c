#define _GNU_SOURCE
#include "desktop.h"
#include <gio/gio.h>
#include <pulse/pulseaudio.h>
#include <pthread.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Only the worker touches PulseAudio and D-Bus. HTTP and the realtime-control
 * loop exchange immutable JSON snapshots and bounded commands with it. */
static struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool stop, started;
    struct json_object *snapshot,*queue[32];
    unsigned queued, generation;
    pa_mainloop *loop;
    pa_context *pulse;
    GDBusConnection *bus;
    struct json_object *mixer,*players;
    char mixer_error[512],player_error[512],action_error[512];
} d={.mutex=PTHREAD_MUTEX_INITIALIZER};
static double stamp(void) { return g_get_monotonic_time()/1e6; }
static struct json_object *get(struct json_object *o,const char *key) {
    struct json_object *v=NULL; json_object_object_get_ex(o,key,&v); return v;
}
static const char *str(struct json_object *o,const char *key) {
    struct json_object *v=get(o,key); return v?json_object_get_string(v):"";
}
static void string(struct json_object *o,const char *key,const char *v) { json_object_object_add(o,key,json_object_new_string(v?v:"")); }
static void boolean(struct json_object *o,const char *key,bool v) { json_object_object_add(o,key,json_object_new_boolean(v)); }
static void number(struct json_object *o,const char *key,double v) { json_object_object_add(o,key,json_object_new_double(v)); }
static bool pulse_wait(pa_operation *op) {
    if (!op) return false;
    double end=stamp()+.6;
    while (pa_operation_get_state(op)==PA_OPERATION_RUNNING && stamp()<end &&
        pa_context_get_state(d.pulse)==PA_CONTEXT_READY) {
        pa_mainloop_iterate(d.loop,0,NULL); usleep(1000);
    }
    bool ok=pa_operation_get_state(op)==PA_OPERATION_DONE;
    if (!ok) pa_operation_cancel(op);
    pa_operation_unref(op); return ok;
}
static bool pulse_connect(void) {
    if (d.pulse && pa_context_get_state(d.pulse)==PA_CONTEXT_READY) return true;
    if (d.pulse) { pa_context_disconnect(d.pulse); pa_context_unref(d.pulse); d.pulse=NULL; }
    d.pulse=pa_context_new(pa_mainloop_get_api(d.loop),"EQ desktop controls");
    if (!d.pulse) return false;
    if (pa_context_connect(d.pulse,NULL,PA_CONTEXT_NOAUTOSPAWN,NULL)<0) return false;
    double end=stamp()+.6;
    while (PA_CONTEXT_IS_GOOD(pa_context_get_state(d.pulse)) && pa_context_get_state(d.pulse)!=PA_CONTEXT_READY && stamp()<end) {
        pa_mainloop_iterate(d.loop,0,NULL); usleep(1000);
    }
    d.generation++;
    return pa_context_get_state(d.pulse)==PA_CONTEXT_READY;
}
static void entry(struct json_object *items,const char *kind,uint32_t id,const char *name,const char *description,
    const pa_cvolume *volume,bool mute,bool writable,pa_proplist *props,uint32_t route) {
    if (json_object_array_length(items)>=512) return;
    struct json_object *o=json_object_new_object(),*channels=json_object_new_array();
    const char *serial=props?pa_proplist_gets(props,"object.serial"):NULL;
    char token[1024]; snprintf(token,sizeof(token),"%u:%s:%u:%s",d.generation,kind,id,serial?serial:"");
    string(o,"token",token); string(o,"kind",kind); string(o,"name",name); string(o,"description",description?description:name);
    string(o,"application",props?pa_proplist_gets(props,PA_PROP_APPLICATION_NAME):NULL);
    json_object_object_add(o,"id",json_object_new_int64(id));
    json_object_object_add(o,"route",json_object_new_int64(route));
    boolean(o,"mute",mute); boolean(o,"writable",writable && pa_cvolume_valid(volume));
    number(o,"volume",pa_cvolume_valid(volume)?100.0*pa_cvolume_max(volume)/PA_VOLUME_NORM:0);
    for (unsigned i=0;i<volume->channels && i<PA_CHANNELS_MAX;i++) json_object_array_add(channels,json_object_new_int64(volume->values[i]));
    json_object_object_add(o,"channels",channels); json_object_array_add(items,o);
}
static void sinks(pa_context *c,const pa_sink_info *i,int end,void *data) {
    (void)c; if (end || !i) return;
    entry(data,"output",i->index,i->name,i->description,&i->volume,i->mute,true,i->proplist,PA_INVALID_INDEX);
}
static void sources(pa_context *c,const pa_source_info *i,int end,void *data) {
    (void)c; if (end || !i) return;
    entry(data,"input",i->index,i->name,i->description,&i->volume,i->mute,true,i->proplist,PA_INVALID_INDEX);
}
static void playback(pa_context *c,const pa_sink_input_info *i,int end,void *data) {
    (void)c; if (end || !i) return;
    pa_cvolume empty={0};
    entry(data,"playback",i->index,i->name,i->name,i->has_volume?&i->volume:&empty,i->mute,i->has_volume && i->volume_writable,i->proplist,i->sink);
}
static void recording(pa_context *c,const pa_source_output_info *i,int end,void *data) {
    (void)c; if (end || !i) return;
    pa_cvolume empty={0};
    entry(data,"recording",i->index,i->name,i->name,i->has_volume?&i->volume:&empty,i->mute,i->has_volume && i->volume_writable,i->proplist,i->source);
}
static void refresh_mixer(void) {
    struct json_object *items=json_object_new_array();
    if (!pulse_connect()) snprintf(d.mixer_error,sizeof(d.mixer_error),"Audio mixer unavailable; check that pipewire-pulse is running.");
    else {
        bool ok=pulse_wait(pa_context_get_sink_info_list(d.pulse,sinks,items));
        ok=pulse_wait(pa_context_get_source_info_list(d.pulse,sources,items)) && ok;
        ok=pulse_wait(pa_context_get_sink_input_info_list(d.pulse,playback,items)) && ok;
        ok=pulse_wait(pa_context_get_source_output_info_list(d.pulse,recording,items)) && ok;
        snprintf(d.mixer_error,sizeof(d.mixer_error),"%s",ok?"":"Audio mixer did not respond; retrying.");
    }
    json_object_put(d.mixer); d.mixer=items;
}
static void success(pa_context *c,int ok,void *data) { (void)c; *(bool *)data=ok; }
static struct json_object *find_token(const char *token) {
    for (size_t i=0;i<json_object_array_length(d.mixer);i++) {
        struct json_object *o=json_object_array_get_idx(d.mixer,i);
        if (!strcmp(str(o,"token"),token)) return o;
    }
    return NULL;
}
static void mixer_action(struct json_object *body) {
    struct json_object *item=find_token(str(body,"token"));
    if (!item || !d.pulse || pa_context_get_state(d.pulse)!=PA_CONTEXT_READY) {
        snprintf(d.action_error,sizeof(d.action_error),"That audio control is no longer available."); return;
    }
    uint32_t id=json_object_get_int64(get(item,"id"));
    const char *kind=str(item,"kind"),*action=str(body,"action");
    pa_operation *op=NULL; bool ok=false;
    if (!strcmp(action,"volume")) {
        if (!json_object_get_boolean(get(item,"writable"))) {
            snprintf(d.action_error,sizeof(d.action_error),"This stream does not support volume changes."); return;
        }
        struct json_object *channels=get(item,"channels");
        pa_cvolume volume={.channels=json_object_array_length(channels)};
        for (unsigned i=0;i<volume.channels;i++) volume.values[i]=json_object_get_int64(json_object_array_get_idx(channels,i));
        pa_cvolume_scale(&volume,lround(json_object_get_double(get(body,"value"))*PA_VOLUME_NORM/100));
        if (!strcmp(kind,"output")) op=pa_context_set_sink_volume_by_index(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"input")) op=pa_context_set_source_volume_by_index(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"playback")) op=pa_context_set_sink_input_volume(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"recording")) op=pa_context_set_source_output_volume(d.pulse,id,&volume,success,&ok);
    } else if (!strcmp(action,"mute")) {
        bool mute=json_object_get_boolean(get(body,"value"));
        if (!strcmp(kind,"output")) op=pa_context_set_sink_mute_by_index(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"input")) op=pa_context_set_source_mute_by_index(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"playback")) op=pa_context_set_sink_input_mute(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"recording")) op=pa_context_set_source_output_mute(d.pulse,id,mute,success,&ok);
    } else if (!strcmp(action,"move")) {
        struct json_object *target=find_token(str(body,"target"));
        if (target && !strcmp(kind,"playback") && !strcmp(str(target,"kind"),"output"))
            op=pa_context_move_sink_input_by_index(d.pulse,id,json_object_get_int64(get(target,"id")),success,&ok);
        if (target && !strcmp(kind,"recording") && !strcmp(str(target,"kind"),"input"))
            op=pa_context_move_source_output_by_index(d.pulse,id,json_object_get_int64(get(target,"id")),success,&ok);
    }
    if (!pulse_wait(op) || !ok) snprintf(d.action_error,sizeof(d.action_error),"Audio change failed: %s",pa_strerror(pa_context_errno(d.pulse)));
    else d.action_error[0]=0;
}
#define PLAYER_PATH "/org/mpris/MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
static GVariant *bus_call(const char *dest,const char *path,const char *iface,const char *method,GVariant *args,GError **error) {
    return g_dbus_connection_call_sync(d.bus,dest,path,iface,method,args,NULL,G_DBUS_CALL_FLAGS_NO_AUTO_START,400,NULL,error);
}
static void variant_string(struct json_object *o,const char *key,GVariant *props,const char *name) {
    const char *s=""; g_variant_lookup(props,name,"&s",&s); string(o,key,s);
}
static void refresh_players(void) {
    struct json_object *players=json_object_new_array();
    GError *error=NULL;
    if (d.bus && g_dbus_connection_is_closed(d.bus)) { g_object_unref(d.bus); d.bus=NULL; }
    if (!d.bus) d.bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);
    GVariant *names=d.bus?bus_call("org.freedesktop.DBus","/org/freedesktop/DBus","org.freedesktop.DBus","ListNames",NULL,&error):NULL;
    if (!names) {
        snprintf(d.player_error,sizeof(d.player_error),"Media players unavailable: %s",error?error->message:"no session bus");
        g_clear_error(&error); goto out;
    }
    d.player_error[0]=0;
    GVariant *array=g_variant_get_child_value(names,0);
    GVariantIter iter; const char *name; g_variant_iter_init(&iter,array);
    while (g_variant_iter_next(&iter,"&s",&name) && json_object_array_length(players)<16) {
        if (!g_str_has_prefix(name,"org.mpris.MediaPlayer2.")) continue;
        GVariant *result=bus_call(name,PLAYER_PATH,"org.freedesktop.DBus.Properties","GetAll",g_variant_new("(s)",PLAYER_IFACE),NULL);
        if (!result) continue;
        GVariant *props=g_variant_get_child_value(result,0);
        struct json_object *p=json_object_new_object(); string(p,"id",name); string(p,"name",name+strlen("org.mpris.MediaPlayer2."));
        variant_string(p,"status",props,"PlaybackStatus");
        const char *caps[]={"CanControl","CanGoNext","CanGoPrevious","CanPlay","CanPause","CanSeek"};
        for (unsigned i=0;i<sizeof(caps)/sizeof(*caps);i++) { gboolean v=FALSE; g_variant_lookup(props,caps[i],"b",&v); boolean(p,caps[i],v); }
        gint64 position=0; g_variant_lookup(props,"Position","x",&position); number(p,"position",position/1e6);
        GVariant *metadata=g_variant_lookup_value(props,"Metadata",G_VARIANT_TYPE("a{sv}"));
        if (metadata) {
            variant_string(p,"title",metadata,"xesam:title"); variant_string(p,"album",metadata,"xesam:album");
            gchar **artists=NULL;
            if (g_variant_lookup(metadata,"xesam:artist","^as",&artists)) {
                char *joined=g_strjoinv(", ",artists); string(p,"artist",joined); g_free(joined); g_strfreev(artists);
            } else string(p,"artist","");
            gint64 length=0; g_variant_lookup(metadata,"mpris:length","x",&length); number(p,"length",length/1e6);
            const char *track=NULL; g_variant_lookup(metadata,"mpris:trackid","o",&track); string(p,"track_id",track);
            g_variant_unref(metadata);
        }
        json_object_array_add(players,p); g_variant_unref(result); /* props is a borrowed child of result */
    }
    g_variant_unref(names); /* array is a borrowed child of names */
out:
    json_object_put(d.players); d.players=players;
}
static void player_action(struct json_object *body) {
    const char *id=str(body,"player"),*action=str(body,"action"),*method=NULL,*cap=NULL;
    if (!strcmp(action,"next")) { method="Next"; cap="CanGoNext"; }
    if (!strcmp(action,"previous")) { method="Previous"; cap="CanGoPrevious"; }
    if (!strcmp(action,"playpause")) method="PlayPause";
    if (!strcmp(action,"seek")) { method="SetPosition"; cap="CanSeek"; }
    if (!strcmp(action,"seek_relative")) { method="Seek"; cap="CanSeek"; }
    struct json_object *player=NULL;
    for (size_t i=0;i<json_object_array_length(d.players);i++) {
        struct json_object *p=json_object_array_get_idx(d.players,i); if (!strcmp(str(p,"id"),id)) player=p;
    }
    if (player && !cap) cap=!strcmp(str(player,"status"),"Playing")?"CanPause":"CanPlay";
    if (!player || !d.bus || !method || !json_object_get_boolean(get(player,"CanControl")) || !json_object_get_boolean(get(player,cap))) {
        snprintf(d.action_error,sizeof(d.action_error),"That player or playback action is unavailable."); return;
    }
    GError *error=NULL;
    GVariant *args=NULL;
    if (!strcmp(action,"seek")) {
        int64_t us=lround(json_object_get_double(get(body,"position"))*1e6);
        args=g_variant_new("(ox)",str(player,"track_id"),us);
    } else if (!strcmp(action,"seek_relative")) {
        int64_t us=lround(json_object_get_double(get(body,"offset"))*1e6);
        args=g_variant_new("(x)",us);
    }
    GVariant *result=bus_call(id,PLAYER_PATH,PLAYER_IFACE,method,args,&error); /* bus_call consumes args */
    if (result) { g_variant_unref(result); d.action_error[0]=0; }
    else snprintf(d.action_error,sizeof(d.action_error),"Player command failed: %s",error?error->message:"no response");
    g_clear_error(&error);
}
static void publish(void) {
    struct json_object *o=json_object_new_object();
    json_object_object_add(o,"mixer",json_object_get(d.mixer)); json_object_object_add(o,"players",json_object_get(d.players));
    string(o,"mixer_error",d.mixer_error); string(o,"player_error",d.player_error); string(o,"action_error",d.action_error);
    pthread_mutex_lock(&d.mutex);
    if (d.snapshot) json_object_put(d.snapshot);
    d.snapshot=o; pthread_mutex_unlock(&d.mutex);
}
static void *worker(void *unused) {
    (void)unused;
    d.loop=pa_mainloop_new(); d.mixer=json_object_new_array(); d.players=json_object_new_array();
    double refresh=0;
    for (;;) {
        pthread_mutex_lock(&d.mutex);
        bool stop=d.stop;
        struct json_object *command=d.queued?d.queue[0]:NULL;
        if (command) { d.queued--; memmove(d.queue,d.queue+1,d.queued*sizeof(*d.queue)); }
        pthread_mutex_unlock(&d.mutex);
        if (stop) { if (command) json_object_put(command); break; }
        if (command) {
            if (!strcmp(str(command,"kind"),"mixer")) mixer_action(command); else player_action(command);
            json_object_put(command); refresh=0;
        }
        if (stamp()>=refresh) { if (d.loop) refresh_mixer(); refresh_players(); publish(); refresh=stamp()+.75; }
        if (d.loop) pa_mainloop_iterate(d.loop,0,NULL);
        usleep(20000);
    }
    if (d.pulse) { pa_context_disconnect(d.pulse); pa_context_unref(d.pulse); }
    if (d.loop) pa_mainloop_free(d.loop);
    if (d.bus) g_object_unref(d.bus);
    json_object_put(d.mixer); json_object_put(d.players); return NULL;
}
int desktop_start(void) {
    int r=pthread_create(&d.thread,NULL,worker,NULL); d.started=r==0; return r;
}
void desktop_stop(void) {
    if (!d.started) return;
    pthread_mutex_lock(&d.mutex); d.stop=true; pthread_mutex_unlock(&d.mutex);
    pthread_join(d.thread,NULL);
    for (unsigned i=0;i<d.queued;i++) json_object_put(d.queue[i]);
    if (d.snapshot) json_object_put(d.snapshot);
}
struct json_object *desktop_snapshot(void) {
    pthread_mutex_lock(&d.mutex);
    struct json_object *o=d.snapshot?json_tokener_parse(json_object_to_json_string(d.snapshot)):json_tokener_parse("{\"mixer\":[],\"players\":[]}");
    pthread_mutex_unlock(&d.mutex); return o;
}
const char *desktop_action(const char *kind,struct json_object *body) {
    struct json_object *a=get(body,"action"),*v=get(body,"value"),*token=get(body,!strcmp(kind,"mixer")?"token":"player");
    if (!json_object_is_type(a,json_type_string) || !json_object_is_type(token,json_type_string) || json_object_get_string_len(token)>1023 ||
        strlen(json_object_get_string(token))!=(size_t)json_object_get_string_len(token)) return "Invalid control identifier or action";
    const char *action=json_object_get_string(a);
    if (!strcmp(kind,"mixer")) {
        if (!strcmp(action,"volume")) {
            if ((!json_object_is_type(v,json_type_double) && !json_object_is_type(v,json_type_int)) ||
                !isfinite(json_object_get_double(v)) || json_object_get_double(v)<0 || json_object_get_double(v)>150) return "Volume must be 0–150 percent";
        } else if (!strcmp(action,"mute")) { if (!json_object_is_type(v,json_type_boolean)) return "Mute must be true or false"; }
        else if (!strcmp(action,"move")) { if (!json_object_is_type(get(body,"target"),json_type_string)) return "Expected a destination"; }
        else return "Unknown mixer action";
    } else if (strcmp(action,"next") && strcmp(action,"previous") && strcmp(action,"playpause") && strcmp(action,"seek") && strcmp(action,"seek_relative")) return "Unknown player action";
    if (!strcmp(kind,"player") && !strcmp(action,"seek")) {
        struct json_object *pos=get(body,"position");
        if ((!json_object_is_type(pos,json_type_double) && !json_object_is_type(pos,json_type_int)) ||
            !isfinite(json_object_get_double(pos)) || json_object_get_double(pos)<0 || json_object_get_double(pos)>=1e9) return "Seek position must be a finite number of seconds";
    }
    if (!strcmp(kind,"player") && !strcmp(action,"seek_relative")) {
        struct json_object *off=get(body,"offset");
        if ((!json_object_is_type(off,json_type_double) && !json_object_is_type(off,json_type_int)) ||
            !isfinite(json_object_get_double(off)) || fabs(json_object_get_double(off))>=1e9) return "Seek offset must be a finite number of seconds";
    }
    pthread_mutex_lock(&d.mutex);
    if (!strcmp(kind,"mixer") && !strcmp(action,"volume")) {
        for (unsigned i=0;i<d.queued;i++) {
            if (!strcmp(str(d.queue[i],"kind"),"mixer") && !strcmp(str(d.queue[i],"action"),"volume") &&
                !strcmp(str(d.queue[i],"token"),json_object_get_string(token))) {
                json_object_object_add(d.queue[i],"value",json_object_new_double(json_object_get_double(v)));
                pthread_mutex_unlock(&d.mutex); return NULL;
            }
        }
    }
    if (d.queued>=32 || !d.started || d.stop) { pthread_mutex_unlock(&d.mutex); return "Desktop controls are busy; try again"; }
    struct json_object *copy=json_tokener_parse(json_object_to_json_string(body));
    string(copy,"kind",kind); d.queue[d.queued++]=copy;
    pthread_mutex_unlock(&d.mutex); return NULL;
}
