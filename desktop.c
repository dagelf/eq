#define _GNU_SOURCE
#include "json_util.h"
#include "desktop.h"
#include "eq.h"
#include <gio/gio.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <pthread.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Only the worker touches PulseAudio and D-Bus. HTTP and the PipeWire control
 * loop exchange immutable JSON snapshots and bounded commands with it. */
static struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    bool stop, started;
    struct json_object *snapshot,*queue[32];
    unsigned queued, generation;
    pa_glib_mainloop *loop;
    GMainContext *context;
    GFileMonitor *audio_monitor, *pulse_monitor;
    void (*notify)(bool reconnect);
    bool mixer_dirty, players_dirty;
    unsigned pulse_attempts;
    double retry_at;
    guint bus_signals[3];
    pa_context *pulse;
    GDBusConnection *bus;
    struct json_object *mixer,*players;
    struct json_object *output_volumes;
    unsigned next_output_epoch, output_epoch;
    char output[NAME_SIZE], master_token[1024], master_error[512];
    bool master_ready;
    char mixer_error[512],player_error[512],action_error[512];
} d={.mutex=PTHREAD_MUTEX_INITIALIZER};
static double stamp(void) { return g_get_monotonic_time()/1e6; }

static void string(struct json_object *o,const char *key,const char *v) { json_object_object_add(o,key,json_object_new_string(v?v:"")); }
static void boolean(struct json_object *o,const char *key,bool v) { json_object_object_add(o,key,json_object_new_boolean(v)); }
static void number(struct json_object *o,const char *key,double v) { json_object_object_add(o,key,json_object_new_double(v)); }
/* Nested iteration is limited to an in-flight operation with a real deadline.
 * Notification callbacks only invalidate state; they never execute commands. */
static gboolean deadline_expired(void *data) { *(bool *)data=true; return G_SOURCE_REMOVE; }
static GSource *deadline(bool *expired) {
    GSource *source=g_timeout_source_new(600);
    g_source_set_callback(source,deadline_expired,expired,NULL);
    g_source_attach(source,d.context); return source;
}
static bool pulse_wait(pa_operation *op) {
    if (!op) return false;
    bool expired=false; GSource *source=deadline(&expired);
    while (pa_operation_get_state(op)==PA_OPERATION_RUNNING && !expired &&
        pa_context_get_state(d.pulse)==PA_CONTEXT_READY) g_main_context_iteration(d.context,TRUE);
    bool ok=pa_operation_get_state(op)==PA_OPERATION_DONE;
    g_source_destroy(source); g_source_unref(source);
    if (!ok) pa_operation_cancel(op);
    pa_operation_unref(op); return ok;
}
static void mixer_changed(pa_context *c,pa_subscription_event_type_t type,uint32_t id,void *data) {
    (void)c; (void)type; (void)id; (void)data; d.mixer_dirty=true;
}
static void pulse_state(pa_context *c,void *data) {
    (void)data;
    if (pa_context_get_state(c)==PA_CONTEXT_FAILED || pa_context_get_state(c)==PA_CONTEXT_TERMINATED)
        d.mixer_dirty=true;
}
static bool pulse_connect(void) {
    if (d.pulse && pa_context_get_state(d.pulse)==PA_CONTEXT_READY) return true;
    if (d.pulse_attempts>=4 || stamp()<d.retry_at) return false;
    d.retry_at=stamp()+(1u<<d.pulse_attempts++);
    if (d.pulse) { pa_context_set_state_callback(d.pulse,NULL,NULL); pa_context_disconnect(d.pulse); pa_context_unref(d.pulse); d.pulse=NULL; }
    d.pulse=pa_context_new(pa_glib_mainloop_get_api(d.loop),"EQ desktop controls");
    if (!d.pulse) return false;
    pa_context_set_state_callback(d.pulse,pulse_state,NULL);
    if (pa_context_connect(d.pulse,NULL,PA_CONTEXT_NOAUTOSPAWN,NULL)<0) return false;
    bool expired=false; GSource *source=deadline(&expired);
    while (PA_CONTEXT_IS_GOOD(pa_context_get_state(d.pulse)) && pa_context_get_state(d.pulse)!=PA_CONTEXT_READY && !expired)
        g_main_context_iteration(d.context,TRUE);
    g_source_destroy(source); g_source_unref(source);
    if (pa_context_get_state(d.pulse)!=PA_CONTEXT_READY) return false;
    d.generation++; d.pulse_attempts=0; d.retry_at=0;
    pa_context_set_subscribe_callback(d.pulse,mixer_changed,NULL);
    return pulse_wait(pa_context_subscribe(d.pulse,PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE|
        PA_SUBSCRIPTION_MASK_SINK_INPUT|PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT,NULL,NULL));
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
        if (!strcmp(json_string(o,"token"),token)) return o;
    }
    return NULL;
}
static void mixer_action(struct json_object *body) {
    struct json_object *item=find_token(json_string(body,"token"));
    if (!item || !d.pulse || pa_context_get_state(d.pulse)!=PA_CONTEXT_READY) {
        snprintf(d.action_error,sizeof(d.action_error),"That audio control is no longer available."); return;
    }
    uint32_t id=json_object_get_int64(json_get(item,"id"));
    const char *kind=json_string(item,"kind"),*action=json_string(body,"action");
    pa_operation *op=NULL; bool ok=false;
    if (!strcmp(action,"volume")) {
        if (!json_object_get_boolean(json_get(item,"writable"))) {
            snprintf(d.action_error,sizeof(d.action_error),"This stream does not support volume changes."); return;
        }
        struct json_object *channels=json_get(item,"channels");
        pa_cvolume volume={.channels=json_object_array_length(channels)};
        for (unsigned i=0;i<volume.channels;i++) volume.values[i]=json_object_get_int64(json_object_array_get_idx(channels,i));
        pa_cvolume_scale(&volume,lround(json_object_get_double(json_get(body,"value"))*PA_VOLUME_NORM/100));
        if (!strcmp(kind,"output")) op=pa_context_set_sink_volume_by_index(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"input")) op=pa_context_set_source_volume_by_index(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"playback")) op=pa_context_set_sink_input_volume(d.pulse,id,&volume,success,&ok);
        if (!strcmp(kind,"recording")) op=pa_context_set_source_output_volume(d.pulse,id,&volume,success,&ok);
    } else if (!strcmp(action,"mute")) {
        bool mute=json_object_get_boolean(json_get(body,"value"));
        if (!strcmp(kind,"output")) op=pa_context_set_sink_mute_by_index(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"input")) op=pa_context_set_source_mute_by_index(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"playback")) op=pa_context_set_sink_input_mute(d.pulse,id,mute,success,&ok);
        if (!strcmp(kind,"recording")) op=pa_context_set_source_output_mute(d.pulse,id,mute,success,&ok);
    } else if (!strcmp(action,"move")) {
        struct json_object *target=find_token(json_string(body,"target"));
        if (target && !strcmp(kind,"playback") && !strcmp(json_string(target,"kind"),"output"))
            op=pa_context_move_sink_input_by_index(d.pulse,id,json_object_get_int64(json_get(target,"id")),success,&ok);
        if (target && !strcmp(kind,"recording") && !strcmp(json_string(target,"kind"),"input"))
            op=pa_context_move_source_output_by_index(d.pulse,id,json_object_get_int64(json_get(target,"id")),success,&ok);
    }
    if (!pulse_wait(op) || !ok) snprintf(d.action_error,sizeof(d.action_error),"Audio change failed: %s",pa_strerror(pa_context_errno(d.pulse)));
    else d.action_error[0]=0;
}
static struct json_object *master_item(void) {
    for (size_t i=0;i<json_object_array_length(d.mixer);i++) {
        struct json_object *item=json_object_array_get_idx(d.mixer,i);
        if (!strcmp(json_string(item,"kind"),"output") && !strcmp(json_string(item,"name"),SINK_NAME)) return item;
    }
    return NULL;
}
static bool restore_master(struct json_object *item,const char *action,struct json_object *value) {
    struct json_object *body=json_object_new_object();
    string(body,"token",json_string(item,"token")); string(body,"action",action);
    json_object_object_add(body,"value",json_object_get(value));
    char previous[sizeof(d.action_error)]; snprintf(previous,sizeof(previous),"%s",d.action_error);
    mixer_action(body);
    snprintf(d.master_error,sizeof(d.master_error),"%s",d.action_error);
    snprintf(d.action_error,sizeof(d.action_error),"%s",previous);
    json_object_put(body); return !*d.master_error;
}
static void sync_master(void) {
    if (!*d.output) return;
    struct json_object *item=master_item();
    if (!item || !json_object_get_boolean(json_get(item,"writable"))) { d.master_ready=false; return; }
    if (strcmp(d.master_token,json_string(item,"token"))) {
        snprintf(d.master_token,sizeof(d.master_token),"%s",json_string(item,"token")); d.master_ready=false;
    }
    char key[NAME_SIZE]; output_key(d.output,key);
    struct json_object *saved=json_get(d.output_volumes,key);
    if (!d.master_ready && saved) {
        /* Apply attenuation before unmuting a restored output. */
        bool muted=json_object_get_boolean(json_get(saved,"mute"));
        if (muted && !restore_master(item,"mute",json_get(saved,"mute"))) return;
        if (!restore_master(item,"volume",json_get(saved,"volume"))) return;
        if (!muted && !restore_master(item,"mute",json_get(saved,"mute"))) return;
        refresh_mixer(); item=master_item();
        if (!item || fabs(json_object_get_double(json_get(item,"volume"))-json_object_get_double(json_get(saved,"volume")))>.01 ||
            json_object_get_boolean(json_get(item,"mute"))!=muted) {
            snprintf(d.master_error,sizeof(d.master_error),"Waiting for the output's saved master volume and mute"); return;
        }
    }
    d.master_ready=true; d.master_error[0]=0;
    struct json_object *value=json_object_new_object();
    json_object_object_add(value,"volume",json_object_get(json_get(item,"volume")));
    json_object_object_add(value,"mute",json_object_get(json_get(item,"mute")));
    json_object_object_add(d.output_volumes,key,value);
}
#define PLAYER_PATH "/org/mpris/MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
static GVariant *bus_call(const char *dest,const char *path,const char *iface,const char *method,GVariant *args,GError **error) {
    return g_dbus_connection_call_sync(d.bus,dest,path,iface,method,args,NULL,G_DBUS_CALL_FLAGS_NO_AUTO_START,400,NULL,error);
}
static void variant_string(struct json_object *o,const char *key,GVariant *props,const char *name) {
    const char *s=""; g_variant_lookup(props,name,"&s",&s); string(o,key,s);
}
static void player_changed(GDBusConnection *bus,const gchar *sender,const gchar *path,
    const gchar *iface,const gchar *signal,GVariant *args,gpointer data) {
    (void)bus; (void)sender; (void)path; (void)iface; (void)data;
    if (!strcmp(signal,"NameOwnerChanged")) {
        const char *name; g_variant_get_child(args,0,"&s",&name);
        if (!g_str_has_prefix(name,"org.mpris.MediaPlayer2.")) return;
    }
    d.players_dirty=true;
}
static void bus_closed(GDBusConnection *bus,gboolean vanished,GError *error,void *data) {
    (void)bus; (void)vanished; (void)error; (void)data; d.players_dirty=true;
}
static void refresh_players(void) {
    struct json_object *players=json_object_new_array();
    GError *error=NULL;
    if (d.bus && g_dbus_connection_is_closed(d.bus)) { g_object_unref(d.bus); d.bus=NULL; }
    if (!d.bus) {
        d.bus=g_bus_get_sync(G_BUS_TYPE_SESSION,NULL,&error);
        if (d.bus) {
            g_dbus_connection_set_exit_on_close(d.bus,FALSE);
            g_signal_connect(d.bus,"closed",G_CALLBACK(bus_closed),NULL);
            d.bus_signals[0]=g_dbus_connection_signal_subscribe(d.bus,"org.freedesktop.DBus","org.freedesktop.DBus",
                "NameOwnerChanged","/org/freedesktop/DBus","org.mpris.MediaPlayer2",G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,player_changed,NULL,NULL);
            d.bus_signals[1]=g_dbus_connection_signal_subscribe(d.bus,NULL,"org.freedesktop.DBus.Properties",
                "PropertiesChanged",PLAYER_PATH,PLAYER_IFACE,G_DBUS_SIGNAL_FLAGS_NONE,player_changed,NULL,NULL);
            d.bus_signals[2]=g_dbus_connection_signal_subscribe(d.bus,NULL,PLAYER_IFACE,
                "Seeked",PLAYER_PATH,NULL,G_DBUS_SIGNAL_FLAGS_NONE,player_changed,NULL,NULL);
        }
    }
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
        gint64 position=0; g_variant_lookup(props,"Position","x",&position); number(p,"position",position/1e6); number(p,"observed_at",g_get_real_time()/1e6);
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
        json_object_array_add(players,p); g_variant_unref(props); g_variant_unref(result);
    }
    g_variant_unref(array); g_variant_unref(names);
out:
    json_object_put(d.players); d.players=players;
}
static void player_action(struct json_object *body) {
    const char *id=json_string(body,"player"),*action=json_string(body,"action"),*method=NULL,*cap=NULL;
    if (!strcmp(action,"next")) { method="Next"; cap="CanGoNext"; }
    if (!strcmp(action,"previous")) { method="Previous"; cap="CanGoPrevious"; }
    if (!strcmp(action,"playpause")) method="PlayPause";
    if (!strcmp(action,"seek")) { method="SetPosition"; cap="CanSeek"; }
    if (!strcmp(action,"seek_relative")) { method="Seek"; cap="CanSeek"; }
    struct json_object *player=NULL;
    for (size_t i=0;i<json_object_array_length(d.players);i++) {
        struct json_object *p=json_object_array_get_idx(d.players,i); if (!strcmp(json_string(p,"id"),id)) player=p;
    }
    if (player && !cap) cap=!strcmp(json_string(player,"status"),"Playing")?"CanPause":"CanPlay";
    if (!player || !d.bus || !method || !json_object_get_boolean(json_get(player,"CanControl")) || !json_object_get_boolean(json_get(player,cap))) {
        snprintf(d.action_error,sizeof(d.action_error),"That player or playback action is unavailable."); return;
    }
    GError *error=NULL;
    GVariant *args=NULL;
    if (!strcmp(action,"seek")) {
        int64_t us=lround(json_object_get_double(json_get(body,"position"))*1e6);
        args=g_variant_new("(ox)",json_string(player,"track_id"),us);
    } else if (!strcmp(action,"seek_relative")) {
        int64_t us=lround(json_object_get_double(json_get(body,"offset"))*1e6);
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
    string(o,"master_error",d.master_error); string(o,"master_output",d.output);
    boolean(o,"master_ready",d.master_ready);
    json_object_object_add(o,"master_epoch",json_object_new_int64(d.output_epoch));
    json_object_object_add(o,"output_volumes",json_copy(d.output_volumes));
    pthread_mutex_lock(&d.mutex);
    if (d.snapshot) json_object_put(d.snapshot);
    d.snapshot=o; pthread_mutex_unlock(&d.mutex);
    d.notify(false);
}
static void socket_changed(GFileMonitor *monitor,GFile *file,GFile *other,GFileMonitorEvent event,void *data) {
    (void)file; (void)other; (void)data;
    if (event!=G_FILE_MONITOR_EVENT_CREATED && event!=G_FILE_MONITOR_EVENT_DELETED &&
        event!=G_FILE_MONITOR_EVENT_MOVED_IN && event!=G_FILE_MONITOR_EVENT_MOVED_OUT) return;
    /* pipewire-pulse may keep its socket while its PipeWire connection is down. */
    d.pulse_attempts=0; d.retry_at=0; d.mixer_dirty=true;
    if (monitor==d.audio_monitor) d.notify(true);
}
static GFileMonitor *watch_socket(const char *path) {
    GFile *file=g_vfs_get_file_for_path(g_vfs_get_local(),path);
    GFileMonitor *monitor=g_file_monitor_file(file,G_FILE_MONITOR_NONE,NULL,NULL);
    g_object_unref(file);
    if (monitor) g_signal_connect(monitor,"changed",G_CALLBACK(socket_changed),NULL);
    return monitor;
}
static void *worker(void *unused) {
    (void)unused;
    g_main_context_push_thread_default(d.context);
    d.loop=pa_glib_mainloop_new(d.context); d.mixer=json_object_new_array(); d.players=json_object_new_array();
    const char *runtime=getenv("PIPEWIRE_RUNTIME_DIR");
    if (!runtime || !*runtime) runtime=getenv("XDG_RUNTIME_DIR");
    const char *remote=getenv("PIPEWIRE_REMOTE"); if (!remote || !*remote) remote="pipewire-0";
    char path[4096];
    if (*remote=='/') d.audio_monitor=watch_socket(remote);
    else if (runtime) { snprintf(path,sizeof(path),"%s/%s",runtime,remote); d.audio_monitor=watch_socket(path); }
    const char *server=getenv("PULSE_SERVER"); runtime=getenv("XDG_RUNTIME_DIR");
    if (server && g_str_has_prefix(server,"unix:")) d.pulse_monitor=watch_socket(server+5);
    else if (runtime) { snprintf(path,sizeof(path),"%s/pulse/native",runtime); d.pulse_monitor=watch_socket(path); }
    d.mixer_dirty=d.players_dirty=true;
    for (;;) {
        while (g_main_context_pending(d.context)) g_main_context_iteration(d.context,FALSE);
        pthread_mutex_lock(&d.mutex);
        bool stop=d.stop;
        struct json_object *command=d.queued?d.queue[0]:NULL;
        if (command) { d.queued--; memmove(d.queue,d.queue+1,d.queued*sizeof(*d.queue)); }
        pthread_mutex_unlock(&d.mutex);
        bool update=command || d.mixer_dirty || d.players_dirty;
        if (d.mixer_dirty) { d.mixer_dirty=false; refresh_mixer(); sync_master(); }
        if (command) {
            const char *kind=json_string(command,"kind");
            if (!strcmp(kind,"output")) {
                snprintf(d.output,sizeof(d.output),"%s",json_string(command,"name"));
                d.output_epoch=json_object_get_int64(json_get(command,"epoch")); d.master_ready=false;
                sync_master();
            } else if (!strcmp(kind,"mixer")) {
                struct json_object *item=find_token(json_string(command,"token"));
                if (item && item==master_item() && !d.master_ready) snprintf(d.action_error,sizeof(d.action_error),"EQ master volume is not ready; try again");
                else mixer_action(command);
                refresh_mixer(); sync_master();
            } else { player_action(command); d.players_dirty=true; }
            json_object_put(command);
        }
        if (d.players_dirty) { d.players_dirty=false; refresh_players(); }
        if (update || stop) publish();
        if (stop && !command) break;
        if (command || d.mixer_dirty || d.players_dirty) continue;
        /* Queue checks and wakeups share the context's persistent wakeup fd. */
        pthread_mutex_lock(&d.mutex); bool pending=d.queued || d.stop; pthread_mutex_unlock(&d.mutex);
        if (pending) continue;
        GSource *retry=NULL; bool expired=false;
        if (d.pulse_attempts>0 && d.pulse_attempts<4) {
            retry=g_timeout_source_new((guint)fmax(1,ceil((d.retry_at-stamp())*1000)));
            g_source_set_callback(retry,deadline_expired,&expired,NULL); g_source_attach(retry,d.context);
        }
        g_main_context_iteration(d.context,TRUE);
        if (retry) { g_source_destroy(retry); g_source_unref(retry); }
        if (expired) d.mixer_dirty=true;
    }
    if (d.audio_monitor) g_object_unref(d.audio_monitor);
    if (d.pulse_monitor) g_object_unref(d.pulse_monitor);
    if (d.pulse) { pa_context_set_state_callback(d.pulse,NULL,NULL); pa_context_disconnect(d.pulse); pa_context_unref(d.pulse); }
    if (d.loop) pa_glib_mainloop_free(d.loop);
    if (d.bus) {
        for (unsigned i=0;i<3;i++) g_dbus_connection_signal_unsubscribe(d.bus,d.bus_signals[i]);
        g_signal_handlers_disconnect_by_func(d.bus,bus_closed,NULL); g_object_unref(d.bus);
    }
    g_main_context_pop_thread_default(d.context);
    json_object_put(d.mixer); json_object_put(d.players); return NULL;
}
int desktop_start(struct json_object *output_volumes,void (*notify)(bool reconnect)) {
    d.notify=notify; d.context=g_main_context_new();
    d.output_volumes=output_volumes?json_copy(output_volumes):json_object_new_object();
    int r=pthread_create(&d.thread,NULL,worker,NULL); d.started=r==0;
    if (r) { json_object_put(d.output_volumes); d.output_volumes=NULL; g_main_context_unref(d.context); }
    return r;
}
struct json_object *desktop_stop(void) {
    if (!d.started) return NULL;
    pthread_mutex_lock(&d.mutex); d.stop=true; pthread_mutex_unlock(&d.mutex);
    g_main_context_wakeup(d.context);
    pthread_join(d.thread,NULL); g_main_context_unref(d.context);
    for (unsigned i=0;i<d.queued;i++) json_object_put(d.queue[i]);
    if (d.snapshot) json_object_put(d.snapshot);
    struct json_object *volumes=d.output_volumes; d.output_volumes=NULL; return volumes;
}
const char *desktop_select_output(const char *output,unsigned *epoch) {
    pthread_mutex_lock(&d.mutex);
    if (d.queued>=32 || !d.started || d.stop) { pthread_mutex_unlock(&d.mutex); return "Desktop controls are busy; try again"; }
    struct json_object *command=json_object_new_object();
    string(command,"kind","output"); string(command,"name",output);
    *epoch=++d.next_output_epoch;
    json_object_object_add(command,"epoch",json_object_new_int64(*epoch));
    d.queue[d.queued++]=command;
    g_main_context_wakeup(d.context);
    pthread_mutex_unlock(&d.mutex); return NULL;
}
struct json_object *desktop_snapshot(void) {
    pthread_mutex_lock(&d.mutex);
    struct json_object *o=d.snapshot?json_copy(d.snapshot):json_tokener_parse("{\"mixer\":[],\"players\":[]}");
    pthread_mutex_unlock(&d.mutex); return o;
}
const char *desktop_action(const char *kind,struct json_object *body) {
    struct json_object *a=json_get(body,"action"),*v=json_get(body,"value"),*token=json_get(body,!strcmp(kind,"mixer")?"token":"player");
    if (!json_object_is_type(a,json_type_string) || !json_object_is_type(token,json_type_string) || json_object_get_string_len(token)>1023 ||
        strlen(json_object_get_string(token))!=(size_t)json_object_get_string_len(token)) return "Invalid control identifier or action";
    const char *action=json_object_get_string(a);
    if (!strcmp(kind,"mixer")) {
        if (!strcmp(action,"volume")) {
            if ((!json_object_is_type(v,json_type_double) && !json_object_is_type(v,json_type_int)) ||
                !isfinite(json_object_get_double(v)) || json_object_get_double(v)<0 || json_object_get_double(v)>150) return "Volume must be 0–150 percent";
        } else if (!strcmp(action,"mute")) { if (!json_object_is_type(v,json_type_boolean)) return "Mute must be true or false"; }
        else if (!strcmp(action,"move")) { if (!json_object_is_type(json_get(body,"target"),json_type_string)) return "Expected a destination"; }
        else return "Unknown mixer action";
    } else if (strcmp(action,"next") && strcmp(action,"previous") && strcmp(action,"playpause") && strcmp(action,"seek") && strcmp(action,"seek_relative")) return "Unknown player action";
    if (!strcmp(kind,"player") && !strcmp(action,"seek")) {
        struct json_object *pos=json_get(body,"position");
        if ((!json_object_is_type(pos,json_type_double) && !json_object_is_type(pos,json_type_int)) ||
            !isfinite(json_object_get_double(pos)) || json_object_get_double(pos)<0 || json_object_get_double(pos)>=1e9) return "Seek position must be a finite number of seconds";
    }
    if (!strcmp(kind,"player") && !strcmp(action,"seek_relative")) {
        struct json_object *off=json_get(body,"offset");
        if ((!json_object_is_type(off,json_type_double) && !json_object_is_type(off,json_type_int)) ||
            !isfinite(json_object_get_double(off)) || fabs(json_object_get_double(off))>=1e9) return "Seek offset must be a finite number of seconds";
    }
    pthread_mutex_lock(&d.mutex);
    if (!strcmp(kind,"mixer") && !strcmp(action,"volume")) {
        for (unsigned n=d.queued;n>0;n--) {
            unsigned i=n-1;
            if (!strcmp(json_string(d.queue[i],"kind"),"output")) break;
            if (!strcmp(json_string(d.queue[i],"kind"),"mixer") && !strcmp(json_string(d.queue[i],"action"),"volume") &&
                !strcmp(json_string(d.queue[i],"token"),json_object_get_string(token))) {
                json_object_object_add(d.queue[i],"value",json_object_new_double(json_object_get_double(v)));
                g_main_context_wakeup(d.context);
    pthread_mutex_unlock(&d.mutex); return NULL;
            }
        }
    }
    if (d.queued>=32 || !d.started || d.stop) { pthread_mutex_unlock(&d.mutex); return "Desktop controls are busy; try again"; }
    struct json_object *copy=json_copy(body);
    string(copy,"kind",kind); d.queue[d.queued++]=copy;
    g_main_context_wakeup(d.context);
    pthread_mutex_unlock(&d.mutex); return NULL;
}
