#define _GNU_SOURCE
#include "eq.h"
#include "desktop.h"
#include "library.h"
#include "json_util.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <microhttpd.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pthread.h>
#include <signal.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include "build/web.h"
#include "build/js.h"

static char origin_url[64]="http://127.0.0.1:8932", host_name[32]="127.0.0.1:8932";
static unsigned listen_port=8932;
#define BODY_LIMIT 4096
struct object {
    struct object *next;
    uint32_t id, out_node, in_node, out_port;
    void *proxy;
    struct pw_properties *props;
    struct spa_hook listener;
    char name[NAME_SIZE], description[NAME_SIZE];
    bool physical, sink, playback, graph, route_checked;
    enum pw_node_state node_state;
    enum pw_link_state link_state;
};
struct app {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_metadata *metadata;
    struct spa_hook core_listener, registry_listener, metadata_listener;
    struct spa_source *timer;
    struct object *objects;
    uint32_t metadata_id;
    bool broken, need_apply, need_route, default_pending, installed, editing, pw_connected;
    bool persist_pending, eq_pending, output_pending, readback, awaiting_readback;
    bool stopping;
    bool outputs_changed;
    int param_seq, expected_seq, stop_seq;
    double next_connect, changed_at, apply_deadline, stop_deadline, default_deadline, output_deadline;
    struct settings desired, saved, actual, sent;
    unsigned revision, saved_revision, applied_revision, sent_revision;
    unsigned master_epoch;
    char state_path[PATH_MAX], config_path[PATH_MAX];
    char persist_error[512], apply_error[512], output_error[512], default_error[512], default_name[NAME_SIZE];
    char instance[64];
    pthread_mutex_t mutex;
    struct json_object *snapshot;
    struct json_object *desktop;
    struct library library;
    double next_desktop;
};
static struct app app={.mutex=PTHREAD_MUTEX_INITIALIZER};
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec/1e9; }
static const char *prop(const struct spa_dict *p,const char *key) { const char *v=p?spa_dict_lookup(p,key):NULL; return v?v:""; }
static bool physical_output(const struct spa_dict *props,const char *name) {
    return !strcmp(prop(props,PW_KEY_MEDIA_CLASS),"Audio/Sink") &&
        strcmp(prop(props,"node.virtual"),"true") &&
        (*prop(props,"device.id") || *prop(props,"device.api")) && valid_name(name);
}
static struct object *find_name(const char *name) {
    for (struct object *o=app.objects;o;o=o->next) if (!strcmp(o->name,name)) return o;
    return NULL;
}
static bool capture_node(uint32_t id) {
    for (struct object *o=app.objects;o;o=o->next)
        if (o->id==id && o->props)
            return !strcmp(prop(&o->props->dict,PW_KEY_MEDIA_CLASS),"Stream/Input/Audio");
    return false;
}
static const char *select_output(const char *name) {
    if (strcmp(app.desired.output,name)) {
        const char *error=desktop_select_output(name,&app.master_epoch);
        if (error) return error;
        library_select_output(&app.library,&app.desired,name);
        if (app.desktop) library_follow(&app.library,app.desktop,&app.desired,true);
        app.eq_pending=true;
    }
    app.output_pending=true;
    return NULL;
}
static void recover_output(void) {
    if (!app.installed || !app.pw_connected || !app.outputs_changed) return;
    struct object *replacement=NULL;
    struct json_object *priority=json_get(app.library.data,"output_priority");
    for (size_t i=0;i<json_object_array_length(priority);i++) {
        const char *name=json_object_get_string(json_object_array_get_idx(priority,i));
        replacement=find_name(name);
        if (replacement && replacement->physical) break;
        replacement=NULL;
        bool ambiguous=false;
        for (struct object *o=app.objects;o;o=o->next) {
            if (!o->physical || !same_bluetooth_output(name,o->name)) continue;
            if (replacement) { ambiguous=true; break; }
            replacement=o;
        }
        if (!ambiguous && replacement) break;
        replacement=NULL;
    }
    app.outputs_changed=false;
    if (!replacement || !strcmp(replacement->name,app.desired.output)) return;
    if (select_output(replacement->name)) { app.outputs_changed=true; return; }
    app.persist_pending=true; app.changed_at=now(); app.revision++;
}
static void route_playback(void) {
    if (!app.editing || !app.metadata || strcmp(app.default_name,SINK_NAME)) return;
    struct object *output=find_name(app.desired.output);
    if (!output || !output->physical) return;
    for (struct object *stream=app.objects;stream;stream=stream->next) {
        if (!stream->props || stream->route_checked || stream->playback) continue;
        const struct spa_dict *props=&stream->props->dict;
        if (strcmp(prop(props,PW_KEY_MEDIA_CLASS),"Stream/Output/Audio") ||
            !strcmp(prop(props,"node.virtual"),"true") || *prop(props,"node.link-group")) continue;
        bool linked=false,other=false;
        for (struct object *link=app.objects;link;link=link->next) {
            if (*link->name || link->out_node!=stream->id ||
                (link->link_state!=PW_LINK_STATE_PAUSED && link->link_state!=PW_LINK_STATE_ACTIVE)) continue;
            linked=true;
            if (link->in_node!=output->id) other=true;
        }
        if (!linked) continue;
        /* Once per stream/default selection. Later manual moves are respected. */
        if (other || pw_metadata_set_property(app.metadata,stream->id,"target.object","Spa:String",SINK_NAME)>=0)
            stream->route_checked=true;
    }
}
static bool readback_same(const struct settings *a,const struct settings *b) {
    if (fabs(a->preamp-b->preamp)>0.002) return false;
    for (int i=0;i<BANDS;i++) if (fabs(a->gains[i]-b->gains[i])>0.002) return false;
    return true;
}
static void node_info(void *data,const struct pw_node_info *info) {
    struct object *o=data;
    if (o->sink && info->state==PW_NODE_STATE_RUNNING && o->node_state!=PW_NODE_STATE_RUNNING && app.awaiting_readback) app.need_apply=true;
    o->node_state=info->state;
    if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && info->props) {
        pw_properties_update(o->props,info->props);
        const struct spa_dict *props=&o->props->dict;
        bool was_graph=o->graph, was_physical=o->physical;
        o->graph=!strcmp(prop(props,"eqweb.version"),"1");
        o->physical=physical_output(props,o->name);
        if (o->sink && o->graph && !was_graph) app.need_apply=true;
        if (o->physical && !was_physical) app.need_route=true;
        if (o->physical!=was_physical) app.outputs_changed=true;
    }
    if (o->sink && (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS))
        pw_node_enum_params(o->proxy,++app.param_seq,SPA_PARAM_Props,0,UINT32_MAX,NULL);
}
static void node_param(void *data,int seq,uint32_t id,uint32_t index,uint32_t next,const struct spa_pod *param) {
    (void)data; (void)seq; (void)id; (void)index; (void)next;
    if (!param) return;
    const struct spa_pod_prop *p=spa_pod_find_prop(param,NULL,SPA_PROP_params);
    if (!p || !spa_pod_is_struct(&p->value)) return;
    const struct spa_pod *v; const char *key=NULL; unsigned mask=0;
    struct settings actual={0};
    SPA_POD_STRUCT_FOREACH(&p->value,v) {
        if (!key) { if (spa_pod_get_string(v,&key)<0) return; }
        else {
            float value;
            if (spa_pod_get_float(v,&value)==0) {
                if (!strcmp(key,"preamp:Mult") && value>0) { actual.preamp=20*log10(value); mask|=1<<BANDS; }
                for (int i=0;i<BANDS;i++) { char name[32]; snprintf(name,sizeof(name),"band%d:Gain",i+1);
                    if (!strcmp(key,name)) { actual.gains[i]=value; mask|=1<<i; }
                }
            }
            key=NULL;
        }
    }
    if (mask==((1u<<(BANDS+1))-1)) {
        app.actual=actual; app.readback=true;
        if (readback_same(&actual,&app.sent)) {
            app.applied_revision=app.sent_revision; app.apply_deadline=0; app.awaiting_readback=false; app.apply_error[0]=0;
        }
    }
}
static const struct pw_node_events node_events={PW_VERSION_NODE_EVENTS,.info=node_info,.param=node_param};
static void link_info(void *data,const struct pw_link_info *info) {
    struct object *o=data; o->out_node=info->output_node_id; o->in_node=info->input_node_id;
    o->out_port=info->output_port_id; o->link_state=info->state;
}
static const struct pw_link_events link_events={PW_VERSION_LINK_EVENTS,.info=link_info};
static int metadata_property(void *data,uint32_t subject,const char *key,const char *type,const char *value) {
    (void)data; (void)type;
    if (subject==PW_ID_CORE && (!key || !strcmp(key,"default.audio.sink"))) {
        bool was_default=!strcmp(app.default_name,SINK_NAME);
        app.default_name[0]=0;
        struct json_object *o=value?json_tokener_parse(value):NULL,*name;
        if (o && json_object_object_get_ex(o,"name",&name)) snprintf(app.default_name,sizeof(app.default_name),"%s",json_object_get_string(name));
        if (!strcmp(app.default_name,SINK_NAME)) { app.default_deadline=0; app.default_error[0]=0; }
        if (!was_default && !strcmp(app.default_name,SINK_NAME))
            for (struct object *n=app.objects;n;n=n->next) n->route_checked=false;
        if (o) json_object_put(o);
    }
    return 0;
}
static const struct pw_metadata_events metadata_events={PW_VERSION_METADATA_EVENTS,.property=metadata_property};
static void registry_global(void *data,uint32_t id,uint32_t permissions,const char *type,uint32_t version,const struct spa_dict *props) {
    (void)data; (void)permissions; (void)version;
    if (!strcmp(type,PW_TYPE_INTERFACE_Metadata) && !strcmp(prop(props,"metadata.name"),"default") && !app.metadata) {
        app.metadata=pw_registry_bind(app.registry,id,type,PW_VERSION_METADATA,0); app.metadata_id=id;
        if (app.metadata) pw_metadata_add_listener(app.metadata,&app.metadata_listener,&metadata_events,NULL);
        app.need_route=true; return;
    }
    bool node=!strcmp(type,PW_TYPE_INTERFACE_Node), link=!strcmp(type,PW_TYPE_INTERFACE_Link);
    if (!node && !link) return;
    struct object *o=calloc(1,sizeof(*o)); if (!o) return;
    o->id=id; o->node_state=PW_NODE_STATE_CREATING; o->link_state=PW_LINK_STATE_INIT;
    if (node) {
        o->props=pw_properties_new_dict(props);
        snprintf(o->name,sizeof(o->name),"%s",prop(props,PW_KEY_NODE_NAME));
        snprintf(o->description,sizeof(o->description),"%s",prop(props,PW_KEY_NODE_DESCRIPTION));
        o->sink=!strcmp(o->name,SINK_NAME); o->playback=!strcmp(o->name,PLAY_NAME);
        o->graph=!strcmp(prop(props,"eqweb.version"),"1");
        o->physical=physical_output(props,o->name);
        o->proxy=pw_registry_bind(app.registry,id,type,PW_VERSION_NODE,0);
        if (o->proxy) pw_node_add_listener(o->proxy,&o->listener,&node_events,o);
        if (o->sink) { app.need_apply=true; app.readback=false; }
        if (o->playback || o->physical) app.need_route=true;
        if (o->physical) app.outputs_changed=true;
    } else {
        o->proxy=pw_registry_bind(app.registry,id,type,PW_VERSION_LINK,0);
        if (o->proxy) pw_link_add_listener(o->proxy,&o->listener,&link_events,o);
    }
    o->next=app.objects; app.objects=o;
}
static void registry_remove(void *data,uint32_t id) {
    (void)data;
    if (app.metadata && id==app.metadata_id) {
        spa_hook_remove(&app.metadata_listener); pw_proxy_destroy((struct pw_proxy *)app.metadata); app.metadata=NULL; app.default_name[0]=0;
    }
    for (struct object **p=&app.objects;*p;p=&(*p)->next) if ((*p)->id==id) {
        struct object *o=*p; *p=o->next;
        if (o->physical) app.outputs_changed=true;
        if (o->sink) app.readback=false;
        if (o->proxy) { spa_hook_remove(&o->listener); pw_proxy_destroy(o->proxy); }
        if (o->props) pw_properties_free(o->props);
        free(o); break;
    }
}
static const struct pw_registry_events registry_events={PW_VERSION_REGISTRY_EVENTS,.global=registry_global,.global_remove=registry_remove};
static void core_error(void *data,uint32_t id,int seq,int res,const char *message) {
    (void)data; (void)seq;
    if (id==PW_ID_CORE && (res==-EPIPE || res==-ECONNRESET)) app.broken=true;
    snprintf(app.apply_error,sizeof(app.apply_error),"PipeWire: %s (%d)",message,res);
}
static void core_info(void *data,const struct pw_core_info *info) { (void)data; (void)info; app.pw_connected=true; }
static void core_done(void *data,uint32_t id,int seq) {
    (void)data;
    if (app.stopping && id==PW_ID_CORE && seq==app.stop_seq) pw_main_loop_quit(app.loop);
}
static const struct pw_core_events core_events={PW_VERSION_CORE_EVENTS,.info=core_info,.error=core_error,.done=core_done};
static void disconnect_pw(void) {
    while (app.objects) registry_remove(NULL,app.objects->id);
    if (app.metadata) registry_remove(NULL,app.metadata_id);
    if (app.registry) { spa_hook_remove(&app.registry_listener); pw_proxy_destroy((struct pw_proxy *)app.registry); app.registry=NULL; }
    if (app.core) { spa_hook_remove(&app.core_listener); pw_core_disconnect(app.core); app.core=NULL; }
    app.pw_connected=false; app.broken=false; app.readback=false; app.apply_deadline=0; app.output_deadline=0;
}
static void connect_pw(void) {
    app.next_connect=now()+2;
    app.core=pw_context_connect(app.context,pw_properties_new(PW_KEY_APP_NAME,"EQ Web Editor",NULL),0);
    if (!app.core) return;
    pw_core_add_listener(app.core,&app.core_listener,&core_events,NULL);
    app.registry=pw_core_get_registry(app.core,PW_VERSION_REGISTRY,0);
    if (!app.registry) { app.broken=true; return; }
    pw_registry_add_listener(app.registry,&app.registry_listener,&registry_events,NULL);
    app.need_apply=true; app.need_route=true;
}
static void apply_eq(struct object *sink) {
    uint8_t buf[2048]; struct spa_pod_builder b=SPA_POD_BUILDER_INIT(buf,sizeof(buf));
    struct spa_pod_frame obj,params;
    spa_pod_builder_push_object(&b,&obj,SPA_TYPE_OBJECT_Props,SPA_PARAM_Props);
    spa_pod_builder_prop(&b,SPA_PROP_params,0); spa_pod_builder_push_struct(&b,&params);
    spa_pod_builder_string(&b,"preamp:Mult"); spa_pod_builder_float(&b,pow(10,app.desired.preamp/20));
    for (int i=0;i<BANDS;i++) { char name[32]; snprintf(name,sizeof(name),"band%d:Gain",i+1);
        spa_pod_builder_string(&b,name); spa_pod_builder_float(&b,app.desired.gains[i]); }
    spa_pod_builder_pop(&b,&params);
    const struct spa_pod *pod=spa_pod_builder_pop(&b,&obj);
    int r=pw_node_set_param(sink->proxy,SPA_PARAM_Props,0,pod);
    if (r<0) snprintf(app.apply_error,sizeof(app.apply_error),"Set EQ parameters: %s",spa_strerror(r));
    else {
        app.sent=app.desired; app.sent_revision=app.revision; app.awaiting_readback=true;
        app.apply_deadline=sink->node_state==PW_NODE_STATE_RUNNING?now()+2:0;
        app.expected_seq=++app.param_seq;
        pw_node_enum_params(sink->proxy,app.expected_seq,SPA_PARAM_Props,0,UINT32_MAX,NULL);
    }
    app.need_apply=false; app.eq_pending=false;
}
static void persist(void) {
    if (save_settings(app.state_path,&app.desired)) {
        snprintf(app.persist_error,sizeof(app.persist_error),"State file: %s",strerror(errno)); return;
    }
    app.saved=app.desired;
    if (!installed_config(app.config_path)) {
        snprintf(app.persist_error,sizeof(app.persist_error),"Configuration missing or replaced; run setup explicitly."); return;
    }
    if (save_config(app.config_path,&app.desired)) {
        snprintf(app.persist_error,sizeof(app.persist_error),"State saved; PipeWire configuration: %s",strerror(errno)); return;
    }
    app.saved_revision=app.revision; app.persist_error[0]=0; app.persist_pending=false;
}
static void add_string(struct json_object *o,const char *key,const char *v) { json_object_object_add(o,key,json_object_new_string(v)); }
static void add_bool(struct json_object *o,const char *key,bool v) { json_object_object_add(o,key,json_object_new_boolean(v)); }
static void snapshot(void) {
    struct object *sink=find_name(SINK_NAME),*play=find_name(PLAY_NAME),*output=find_name(app.desired.output);
    unsigned links=0; uint32_t first_port=SPA_ID_INVALID; bool wrong=false,link_error=false;
    for (struct object *o=app.objects;o;o=o->next) if (play && o->out_node==play->id && !*o->name) {
        /* Recording and volume-meter taps do not change the playback route. */
        if (capture_node(o->in_node)) continue;
        if (o->link_state==PW_LINK_STATE_ERROR) link_error=true;
        if (o->link_state!=PW_LINK_STATE_PAUSED && o->link_state!=PW_LINK_STATE_ACTIVE) continue;
        if (output && o->in_node==output->id) { if (!links) { first_port=o->out_port; links=1; } else if (o->out_port!=first_port) links=2; }
        else wrong=true;
    }
    bool connected=app.pw_connected && sink && sink->graph && app.readback && play && output && output->physical && links>=2 && !wrong && !link_error && sink->node_state!=PW_NODE_STATE_ERROR;
    if (connected) { app.output_deadline=0; app.output_error[0]=0; }
    app.editing=connected && app.installed;
    struct json_object *o=json_object_new_object(),*live=json_object_new_object(),*outputs=json_object_new_array(),*freq=json_object_new_array();
    add_string(o,"instance",app.instance);
    json_object_object_add(o,"settings",settings_json(&app.desired));
    json_object_object_add(o,"average_gain",json_object_new_double(average_gain(&app.desired)));
    if (app.desktop) add_bool(app.desktop,"master_ready",json_object_get_boolean(json_get(app.desktop,"master_ready")) &&
        json_object_get_int64(json_get(app.desktop,"master_epoch"))==app.master_epoch &&
        !strcmp(json_string(app.desktop,"master_output"),app.desired.output));
    json_object_object_add(o,"desktop",app.desktop?json_object_get(app.desktop):json_object_new_object());
    json_object_object_add(o,"library",library_snapshot(&app.library,&app.desired));
    json_object_object_add(o,"saved",settings_json(&app.saved));
    for (int i=0;i<BANDS;i++) json_object_array_add(freq,json_object_new_int(frequencies[i]));
    json_object_object_add(o,"frequencies",freq);
    for (struct object *n=app.objects;n;n=n->next) if (n->physical) {
        struct json_object *v=json_object_new_object(); add_string(v,"name",n->name); add_string(v,"description",*n->description?n->description:n->name); json_object_array_add(outputs,v);
    }
    json_object_object_add(o,"outputs",outputs);
    add_bool(live,"pipewire",app.pw_connected); add_bool(live,"sink",sink!=NULL); add_bool(live,"graph_ready",sink && sink->graph && app.readback);
    add_bool(live,"output_present",output && output->physical); add_bool(live,"connected",connected); add_bool(live,"editing",app.editing);
    add_bool(live,"awaiting_readback",app.awaiting_readback);
    add_bool(live,"running",sink && sink->node_state==PW_NODE_STATE_RUNNING);
    add_bool(live,"default",!strcmp(app.default_name,SINK_NAME)); add_bool(live,"can_set_default",connected && app.metadata);
    add_bool(live,"can_select_output",app.pw_connected && sink && sink->graph && play && app.metadata && app.installed);
    json_object_object_add(live,"stereo_links",json_object_new_int(links));
    if (sink) json_object_object_add(live,"sink_id",json_object_new_int64(sink->id));
    if (app.readback) json_object_object_add(live,"eq",settings_json(&app.actual));
    bool output_missing=!output || !output->physical;
    if (output_missing) { app.output_error[0]=0; app.output_deadline=0; }
    add_string(live,"status",!app.pw_connected?"PipeWire disconnected; reconnecting":!app.installed?"Setup required":!sink || !sink->graph?"Restart PipeWire once to load the new EQ graph":sink->node_state==PW_NODE_STATE_ERROR?"EQ is in an error state":!app.readback?"Waiting for EQ parameter readback":output_missing?"EQ output is unavailable. Select your speakers or headphones under Output below; no audio restart is needed. EQ editing is disabled until it reconnects.":wrong?"EQ is connected to a different output. Select the desired device under Output below.":link_error?"EQ could not connect to the selected output. Select an available device under Output below.":links<2?"EQ is not connected to the selected output for both audio channels. Select an available device under Output below; EQ editing is disabled until it reconnects.":"Connected");
    json_object_object_add(o,"live",live);
    add_bool(o,"setup_required",!app.installed);
    add_bool(o,"pending",app.persist_pending || app.eq_pending || app.output_pending || app.default_pending || app.apply_deadline>0 || app.default_deadline>0 || app.output_deadline>0);
    json_object_object_add(o,"revision",json_object_new_int64(app.revision));
    json_object_object_add(o,"saved_revision",json_object_new_int64(app.saved_revision));
    json_object_object_add(o,"applied_revision",json_object_new_int64(app.applied_revision));
    add_string(o,"persistence_error",app.persist_error); add_string(o,"apply_error",app.apply_error); add_string(o,"output_error",app.output_error); add_string(o,"default_error",app.default_error);
    if (app.snapshot) json_object_put(app.snapshot);
    app.snapshot=o;
}
static void tick(void *data,uint64_t expirations) {
    (void)data; (void)expirations; pthread_mutex_lock(&app.mutex);
    double t=now();
    if (app.broken) disconnect_pw();
    if (!app.core && t>=app.next_connect) connect_pw();
    recover_output();
    if (t>=app.next_desktop) {
        if (app.desktop) json_object_put(app.desktop);
        app.desktop=desktop_snapshot();
        library_volumes(&app.library,json_get(app.desktop,"output_volumes"));
        if (library_follow(&app.library,app.desktop,&app.desired,app.editing)) {
            app.revision++; app.changed_at=t; app.persist_pending=true; app.eq_pending=true;
        }
        app.next_desktop=t+.5;
    }
    library_flush(&app.library,t);
    struct object *sink=find_name(SINK_NAME),*play=find_name(PLAY_NAME);
    if ((app.need_apply || app.eq_pending) && sink && sink->graph && sink->proxy) apply_eq(sink);
    if ((app.need_route || app.output_pending) && play && app.metadata) {
        int r=pw_metadata_set_property(app.metadata,play->id,"target.object","Spa:String",app.desired.output);
        if (r<0) snprintf(app.output_error,sizeof(app.output_error),"Select output: %s",spa_strerror(r));
        else { app.output_error[0]=0; app.output_deadline=t+2; }
        app.need_route=false; app.output_pending=false;
    }
    if (app.default_pending && app.metadata && sink) {
        app.default_error[0]=0;
        if (!strcmp(app.default_name,SINK_NAME)) app.default_deadline=0;
        else {
            int r=pw_metadata_set_property(app.metadata,PW_ID_CORE,"default.configured.audio.sink","Spa:String","{\"name\":\"" SINK_NAME "\"}");
            if (r<0) snprintf(app.default_error,sizeof(app.default_error),"Default sink: %s",spa_strerror(r));
            else app.default_deadline=t+2;
        }
        app.default_pending=false;
    }
    if (app.apply_deadline>0 && sink && sink->node_state!=PW_NODE_STATE_RUNNING) app.apply_deadline=0;
    if (app.apply_deadline>0 && t>app.apply_deadline) {
        snprintf(app.apply_error,sizeof(app.apply_error),"EQ parameter readback did not confirm the requested settings"); app.apply_deadline=0;
    }
    if (app.output_deadline>0 && t>app.output_deadline) {
        snprintf(app.output_error,sizeof(app.output_error),"EQ output connection did not complete. Select an available device under Output below; no audio restart is needed."); app.output_deadline=0;
    }
    if (app.default_deadline>0 && t>app.default_deadline) {
        snprintf(app.default_error,sizeof(app.default_error),"WirePlumber did not confirm EQ as the default sink"); app.default_deadline=0;
    }
    if (app.persist_pending && (app.stopping || t-app.changed_at>=0.35)) { persist(); if (app.persist_pending) app.changed_at=t+1.65; }
    if (app.stopping) {
        if (!app.core || t>app.stop_deadline) pw_main_loop_quit(app.loop);
        else if (!app.stop_seq) app.stop_seq=pw_core_sync(app.core,PW_ID_CORE,0);
    }
    snapshot();
    if (!app.stopping) route_playback();
    pthread_mutex_unlock(&app.mutex);
}
struct request { size_t len; bool oversized; char body[BODY_LIMIT+1]; };
static enum MHD_Result reply(struct MHD_Connection *c,unsigned code,const char *body,const char *type) {
    struct MHD_Response *r=MHD_create_response_from_buffer(strlen(body),(void *)body,MHD_RESPMEM_MUST_COPY);
    if (!r) return MHD_NO;
    MHD_add_response_header(r,"Content-Type",type);
    MHD_add_response_header(r,"Cache-Control","no-store");
    MHD_add_response_header(r,"X-Content-Type-Options","nosniff");
    MHD_add_response_header(r,"Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    enum MHD_Result result=MHD_queue_response(c,code,r); MHD_destroy_response(r); return result;
}
static enum MHD_Result error_reply(struct MHD_Connection *c,unsigned code,const char *msg) {
    struct json_object *o=json_object_new_object(); add_string(o,"error",msg);
    enum MHD_Result r=reply(c,code,json_object_to_json_string(o),"application/json"); json_object_put(o); return r;
}
static struct json_object *parse_body(struct request *r) {
    struct json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    struct json_object *o=json_tokener_parse_ex(tok,r->body,(int)r->len);
    if (json_tokener_get_error(tok)!=json_tokener_success) { if (o) json_object_put(o); o=NULL; }
    json_tokener_free(tok); return o;
}
static enum MHD_Result handle(void *cls,struct MHD_Connection *c,const char *url,const char *method,const char *version,const char *upload,size_t *size,void **context) {
    (void)cls; (void)version;
    if (!*context) { *context=calloc(1,sizeof(struct request)); return *context?MHD_YES:MHD_NO; }
    struct request *r=*context;
    if (*size) {
        if (*size>BODY_LIMIT-r->len) r->oversized=true;
        else if (!r->oversized) { memcpy(r->body+r->len,upload,*size); r->len+=*size; }
        *size=0; return MHD_YES;
    }
    const char *host=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Host");
    if (!host || strcmp(host,host_name)) return error_reply(c,403,origin_url);
    if (r->oversized) return error_reply(c,413,"Request exceeds 4096 bytes");
    if (!strcmp(method,"GET")) {
        if (!strcmp(url,"/")) return reply(c,200,(char *)web_html,"text/html; charset=utf-8");
        if (!strcmp(url,"/app.js")) {
            return reply(c,200,(char *)app_js,"text/javascript; charset=utf-8");
        }
        if (strcmp(url,"/api/state")) return error_reply(c,404,"Not found");
        pthread_mutex_lock(&app.mutex);
        enum MHD_Result result=reply(c,200,json_object_to_json_string(app.snapshot),"application/json");
        pthread_mutex_unlock(&app.mutex); return result;
    }
    const char *origin=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Origin"),*site=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Sec-Fetch-Site");
    if ((origin && strcmp(origin,origin_url)) || (site && strcmp(site,"same-origin") && strcmp(site,"none"))) return error_reply(c,403,"Cross-origin mutations are forbidden");
    bool eq=!strcmp(url,"/api/eq"),output=!strcmp(url,"/api/output"),def=!strcmp(url,"/api/default");
    bool mixer=!strcmp(url,"/api/mixer"),player=!strcmp(url,"/api/player"),library=!strcmp(url,"/api/library");
    if (!eq && !output && !def && !mixer && !player && !library) return error_reply(c,404,"Not found");
    if (strcmp(method,(def || mixer || player || library)?"POST":"PUT")) return error_reply(c,405,"Method not allowed");
    const char *ct=MHD_lookup_connection_value(c,MHD_HEADER_KIND,"Content-Type");
    if (!ct || (strcmp(ct,"application/json") && strcmp(ct,"application/json; charset=utf-8"))) return error_reply(c,415,"Content-Type must be application/json");
    struct json_object *body=parse_body(r);
    if (!body || !json_object_is_type(body,json_type_object)) { if (body) json_object_put(body); return error_reply(c,400,"Expected a JSON object"); }
    pthread_mutex_lock(&app.mutex);
    if (app.stopping) {
        pthread_mutex_unlock(&app.mutex); json_object_put(body);
        return error_reply(c,503,"Editor is shutting down");
    }
    unsigned code=202; const char *error=NULL;
    struct json_object *live,*cap;
    json_object_object_get_ex(app.snapshot,"live",&live);
    if (eq) {
        struct settings next=app.desired;
        bool known=true;
        json_object_object_foreach(body,key,value) { (void)value; if (strcmp(key,"gains") && strcmp(key,"preamp") && strcmp(key,"maintain") && strcmp(key,"loudness_target") && strcmp(key,"output")) known=false; }
        if (!known || !parse_eq(body,&next)) { code=400; error="Expected ten numeric gains (-12..12), preamp (-24..12), and optional loudness settings"; }
        else if (json_get(body,"output") && strcmp(json_string(body,"output"),app.desired.output)) { code=409; error="Output changed; these EQ edits belong to another output"; }
        else if (!app.editing) { code=409; error="EQ is disconnected; check setup and selected output"; }
        else { app.desired=next; app.eq_pending=true; library_remember(&app.library,&next); }
    } else if (mixer || player) {
        if (mixer && json_get(body,"output") && strcmp(json_string(body,"output"),app.desired.output)) { code=409; error="Output changed; adjust its volume again"; }
        else { error=desktop_action(mixer?"mixer":"player",body); if (error) code=400; }
    } else if (library) {
        error=library_action(&app.library,body,&app.desired); if (error) code=400;
        /* Resolve recall preferences before accepting another EQ edit, so its
         * song/device identity and the selected fallback are already current. */
        if (!error && app.desktop && library_follow(&app.library,app.desktop,&app.desired,app.editing)) {
            app.revision++; app.changed_at=now(); app.persist_pending=true; app.eq_pending=true;
        }
    } else if (output) {
        struct json_object *name,*outputs;
        bool valid=json_object_object_length(body)==1 && json_object_object_get_ex(body,"name",&name) && json_object_is_type(name,json_type_string) && valid_name(json_object_get_string(name)) && strlen(json_object_get_string(name))==(size_t)json_object_get_string_len(name);
        bool found=false; json_object_object_get_ex(app.snapshot,"outputs",&outputs);
        if (valid) for (size_t i=0;i<json_object_array_length(outputs);i++) { struct json_object *n; json_object_object_get_ex(json_object_array_get_idx(outputs,i),"name",&n); if (!strcmp(json_object_get_string(n),json_object_get_string(name))) found=true; }
        json_object_object_get_ex(live,"can_select_output",&cap);
        if (!valid) { code=400; error="Expected a physical output node name"; }
        else if (!found) { code=409; error="Selected physical output is unavailable"; }
        else if (!json_object_get_boolean(cap)) { code=409; error="EQ or WirePlumber is unavailable"; }
        else {
            error=select_output(json_object_get_string(name));
            if (error) code=503;
            else library_prefer_output(&app.library,json_object_get_string(name));
        }
    } else {
        json_object_object_get_ex(live,"can_set_default",&cap);
        if (json_object_object_length(body)) { code=400; error="Expected an empty JSON object"; }
        else if (!json_object_get_boolean(cap)) { code=409; error="Connect the EQ to an output first"; }
        else app.default_pending=true;
    }
    if (!error && (eq || output)) { app.revision++; app.changed_at=now(); app.persist_pending=true; }
    unsigned revision=app.revision;
    pthread_mutex_unlock(&app.mutex); json_object_put(body);
    if (error) return error_reply(c,code,error);
    char accepted[96]; snprintf(accepted,sizeof(accepted),"{\"accepted\":true,\"revision\":%u}",revision);
    return reply(c,202,accepted,"application/json");
}
static void completed(void *cls,struct MHD_Connection *c,void **context,enum MHD_RequestTerminationCode code) { (void)cls; (void)c; (void)code; free(*context); *context=NULL; }
static void quit(void *data,int sig) {
    (void)data; (void)sig;
    pthread_mutex_lock(&app.mutex); app.stopping=true; app.stop_deadline=now()+.5; pthread_mutex_unlock(&app.mutex);
}
static int paths(void) {
    const char *home=getenv("HOME"),*state=getenv("XDG_STATE_HOME"),*config=getenv("XDG_CONFIG_HOME");
    if (!home || *home!='/') return -1;
    if (state && *state && *state!='/') return -1;
    if (config && *config && *config!='/') return -1;
    int n=state && *state?snprintf(app.state_path,sizeof(app.state_path),"%s/eqweb/settings-v1.json",state):snprintf(app.state_path,sizeof(app.state_path),"%s/.local/state/eqweb/settings-v1.json",home);
    int m=config && *config?snprintf(app.config_path,sizeof(app.config_path),"%s/pipewire/pipewire.conf.d/99-eqweb.conf",config):snprintf(app.config_path,sizeof(app.config_path),"%s/.config/pipewire/pipewire.conf.d/99-eqweb.conf",home);
    return n>=(int)sizeof(app.state_path) || m>=(int)sizeof(app.config_path)?-1:0;
}
struct discovery { int seq, rounds; };
static void discovery_done(void *data,uint32_t id,int seq) {
    struct discovery *d=data;
    if (id!=PW_ID_CORE || seq!=d->seq) return;
    /* The second round includes properties from nodes bound during discovery. */
    if (++d->rounds==1) d->seq=pw_core_sync(app.core,PW_ID_CORE,0);
}
static const struct pw_core_events discovery_events={PW_VERSION_CORE_EVENTS,.done=discovery_done};
static int output_order(const void *a,const void *b) {
    const struct object *x=*(const struct object *const *)a,*y=*(const struct object *const *)b;
    int r=strcmp(x->description,y->description);
    return r?r:strcmp(x->name,y->name);
}
static int choose_output(bool list_only) {
    int result=1;
    struct object **outputs=NULL;
    pw_init(NULL,NULL);
    app.loop=pw_main_loop_new(NULL);
    if (!app.loop) goto out;
    struct pw_loop *loop=pw_main_loop_get_loop(app.loop);
    app.context=pw_context_new(loop,NULL,0);
    if (!app.context) goto out;
    connect_pw();
    if (!app.core || !app.registry) {
        fputs("Cannot connect to PipeWire. Start your desktop audio service, then run ./eqweb setup again.\n",stderr);
        goto out;
    }
    struct discovery d={.seq=pw_core_sync(app.core,PW_ID_CORE,0)};
    struct spa_hook listener;
    pw_core_add_listener(app.core,&listener,&discovery_events,&d);
    double deadline=now()+3;
    pw_loop_enter(loop);
    while (d.rounds<2 && !app.broken && !*app.apply_error && now()<deadline)
        if (pw_loop_iterate(loop,100)<0) break;
    pw_loop_leave(loop);
    spa_hook_remove(&listener);
    if (d.rounds<2 || app.broken || *app.apply_error) {
        fprintf(stderr,"Cannot list audio outputs: %s\nCheck that PipeWire is running, then try again.\n",*app.apply_error?app.apply_error:"PipeWire did not respond");
        goto out;
    }
    size_t count=0;
    for (struct object *o=app.objects;o;o=o->next) if (o->physical) count++;
    if (!count) {
        fputs("No physical audio outputs found. Connect or enable your speakers, headphones or audio interface, then try again.\n",stderr);
        goto out;
    }
    outputs=calloc(count,sizeof(*outputs));
    if (!outputs) goto out;
    size_t i=0;
    for (struct object *o=app.objects;o;o=o->next) if (o->physical) outputs[i++]=o;
    qsort(outputs,count,sizeof(*outputs),output_order);
    puts("Available audio outputs:");
    for (i=0;i<count;i++) {
        printf("  %zu. %s\n",i+1,*outputs[i]->description?outputs[i]->description:outputs[i]->name);
        if (list_only) printf("     ./eqweb setup --output %s\n",outputs[i]->name);
        else printf("     %s\n",outputs[i]->name);
    }
    if (list_only) { result=0; goto out; }
    for (;;) {
        printf("Choose output [1-%zu], or press Enter to cancel: ",count); fflush(stdout);
        char line[128],*end;
        if (!fgets(line,sizeof(line),stdin) || !strcmp(line,"\n")) {
            puts("Setup cancelled; no configuration changed."); break;
        }
        if (!strchr(line,'\n') && !feof(stdin)) {
            int ch; while ((ch=getchar())!='\n' && ch!=EOF) {}
            puts("Enter a number from the list."); continue;
        }
        errno=0; long selected=strtol(line,&end,10);
        bool number=end!=line;
        while (isspace((unsigned char)*end)) end++;
        if (errno || !number || *end || selected<1 || (unsigned long)selected>count) {
            puts("Enter a number from the list."); continue;
        }
        snprintf(app.desired.output,sizeof(app.desired.output),"%s",outputs[selected-1]->name);
        printf("Selected: %s\n",*outputs[selected-1]->description?outputs[selected-1]->description:app.desired.output);
        result=0; break;
    }
out:
    free(outputs);
    disconnect_pw();
    if (app.context) { pw_context_destroy(app.context); app.context=NULL; }
    if (app.loop) { pw_main_loop_destroy(app.loop); app.loop=NULL; }
    pw_deinit();
    return result;
}
int main(int argc,char **argv) {
    setlocale(LC_NUMERIC,"C");
    const char *port=getenv("EQWEB_PORT");
    if (port) {
        char *end; errno=0; unsigned long n=strtoul(port,&end,10);
        if (errno || end==port || *end || n<1024 || n>65535) { fputs("EQWEB_PORT must be 1024–65535\n",stderr); return 2; }
        listen_port=n; snprintf(host_name,sizeof(host_name),"127.0.0.1:%u",listen_port);
        snprintf(origin_url,sizeof(origin_url),"http://%s",host_name);
    }
    snprintf(app.instance,sizeof(app.instance),"%ld-%.9f",(long)getpid(),now());
    if (paths()) { fputs("HOME and XDG paths must be absolute and fit PATH_MAX\n",stderr); return 1; }
    snprintf(app.desired.output,sizeof(app.desired.output),"eqweb.unselected");
    int loaded=load_settings(app.state_path,&app.desired);
    if (loaded<0) { fprintf(stderr,"Cannot load %s: %s\n",app.state_path,strerror(errno)); return 1; }
    app.saved=app.desired;
    if (argc>1 && !strcmp(argv[1],"--help")) {
        puts("eqweb                            Serve http://127.0.0.1:8932\neqweb setup                      Choose an audio output and install the EQ\neqweb outputs                    List audio outputs and setup commands\neqweb setup --output NAME        Install using an output name from 'eqweb outputs'\neqweb print-config --output NAME Print a graph without installing it\nUses XDG_CONFIG_HOME and XDG_STATE_HOME. Setup requires one PipeWire restart."); return 0;
    }
    if (argc==2 && !strcmp(argv[1],"outputs")) return choose_output(true);
    bool setup=argc>1 && !strcmp(argv[1],"setup"),print=argc>1 && !strcmp(argv[1],"print-config");
    bool choose=setup && (argc==2 || (argc==3 && !strcmp(argv[2],"--output")) ||
        (argc==4 && !strcmp(argv[2],"--output") && !strcmp(argv[3],"?")));
    if (setup || print) {
        if (!choose) {
            if (argc!=4 || strcmp(argv[2],"--output") || !valid_name(argv[3])) {
                fputs("Run ./eqweb setup to choose an audio output by number.\nFor scripts, run ./eqweb outputs and copy the listed --output name.\n",stderr); return 2;
            }
            snprintf(app.desired.output,sizeof(app.desired.output),"%s",argv[3]);
        }
        if (print) { char *s=render_config(&app.desired); if (!s) return 1; fputs(s,stdout); free(s); return 0; }
    } else if (argc!=1) { fputs("Unknown arguments; use --help\n",stderr); return 2; }
    /* A process lock shared by setup and server prevents competing state writers. */
    char lock_path[PATH_MAX];
    if (snprintf(lock_path,sizeof(lock_path),"%s.lock",app.state_path)>=(int)sizeof(lock_path)) return 1;
    /* Create directories through the atomic writer, without touching settings. */
    char init_path[PATH_MAX];
    if (snprintf(init_path,sizeof(init_path),"%s.init.%ld",app.state_path,(long)getpid())>=(int)sizeof(init_path)) return 1;
    if (atomic_write(init_path,"",0)) { perror("Create state directory"); return 1; } unlink(init_path);
    int lock=open(lock_path,O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { fputs("Another eqweb process is running or the state lock is unavailable\n",stderr); return 1; }
    if (library_open(&app.library,app.state_path)) { perror("Load presets and song EQ"); close(lock); return 1; }
    if (setup) {
        if (choose && choose_output(false)) { close(lock); return 1; }
        char backup[PATH_MAX]; int r=backup_config(app.config_path,backup,sizeof(backup));
        if (r<0) { perror("Back up existing configuration"); return 1; }
        if (r) printf("Backup: %s\n",backup);
        if (save_settings(app.state_path,&app.desired) || save_config(app.config_path,&app.desired)) { perror("Install configuration"); return 1; }
        library_prefer_output(&app.library,app.desired.output);
        library_save(&app.library);
        if (app.library.dirty) { fprintf(stderr,"Save output priority: %s\n",app.library.error); return 1; }
        printf("Installed: %s\nRestart desktop audio once to load the graph:\n  command systemctl --user restart pipewire pipewire-pulse wireplumber\nRun as your normal user; 'command' bypasses shell aliases that might add sudo.\nThis interrupts current playback. Ordinary EQ adjustments require no restart.\n",app.config_path); close(lock); return 0;
    }
    app.installed=installed_config(app.config_path);
    if (!json_object_array_length(json_get(app.library.data,"output_priority")))
        library_prefer_output(&app.library,app.desired.output);
    if (app.installed && loaded) { app.persist_pending=true; app.changed_at=now()-1; }
    pw_init(NULL,NULL);
    app.loop=pw_main_loop_new(NULL);
    if (!app.loop) return 1;
    app.context=pw_context_new(pw_main_loop_get_loop(app.loop),NULL,0);
    if (!app.context) return 1;
    pw_loop_add_signal(pw_main_loop_get_loop(app.loop),SIGINT,quit,NULL);
    pw_loop_add_signal(pw_main_loop_get_loop(app.loop),SIGTERM,quit,NULL);
    /* Workers inherit the signal mask installed by the PipeWire loop. */
    library_init_output(&app.library,&app.desired);
    if (desktop_start(json_get(app.library.data,"output_volumes"))) { fputs("Cannot start desktop controls\n",stderr); close(lock); return 1; }
    desktop_select_output(app.desired.output,&app.master_epoch);
    app.timer=pw_loop_add_timer(pw_main_loop_get_loop(app.loop),tick,NULL);
    struct timespec interval={0,50000000};
    pw_loop_update_timer(pw_main_loop_get_loop(app.loop),app.timer,&interval,&interval,false);
    tick(NULL,0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(listen_port),.sin_addr={htonl(INADDR_LOOPBACK)}};
    struct MHD_Daemon *http=MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,listen_port,NULL,NULL,handle,NULL,
        MHD_OPTION_SOCK_ADDR,&addr,MHD_OPTION_NOTIFY_COMPLETED,completed,NULL,
        MHD_OPTION_CONNECTION_LIMIT,(unsigned int)16,MHD_OPTION_CONNECTION_TIMEOUT,(unsigned int)5,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT,(size_t)16384,MHD_OPTION_END);
    if (!http) { fprintf(stderr,"Cannot listen on %s\n",host_name); return 1; }
    printf("EQ editor: %s\n",origin_url); fflush(stdout);
    pw_main_loop_run(app.loop);
    MHD_stop_daemon(http);
    if (app.persist_pending) persist();
    bool failed=app.persist_pending;
    if (failed) fprintf(stderr,"Unsaved changes: %s\n",app.persist_error);
    disconnect_pw(); pw_loop_destroy_source(pw_main_loop_get_loop(app.loop),app.timer);
    pw_context_destroy(app.context); pw_main_loop_destroy(app.loop); pw_deinit();
    struct json_object *volumes=desktop_stop();
    library_volumes(&app.library,volumes); if (volumes) json_object_put(volumes);
    library_close(&app.library);
    if (*app.library.error) { fprintf(stderr,"%s\n",app.library.error); failed=true; }
    if (*app.library.log_error) { fprintf(stderr,"%s\n",app.library.log_error); failed=true; }
    if (app.desktop) json_object_put(app.desktop);
    json_object_put(app.snapshot); close(lock); return failed?1:0;
}
