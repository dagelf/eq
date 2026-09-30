#define _GNU_SOURCE
#include "library.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static struct json_object *json(const char *s) { struct json_object *o=json_tokener_parse(s); assert(o); return o; }
static struct json_object *get(struct json_object *o,const char *key) {
    struct json_object *v=NULL; json_object_object_get_ex(o,key,&v); return v;
}
static void action(struct library *l,const char *s,struct settings *eq) {
    struct json_object *o=json(s); assert(!library_action(l,o,eq)); json_object_put(o);
}
static char *slurp(const char *path) {
    FILE *f=fopen(path,"r"); assert(f); fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
    char *s=calloc((size_t)n+1,1); assert(s && fread(s,1,(size_t)n,f)==(size_t)n); fclose(f); return s;
}
static void regressions(void) {
    char dir[]="/tmp/eqweb-regression-XXXXXX",path[512]; assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/settings.json",dir);
    struct library l={0}; assert(!library_open(&l,path));
    struct settings s={.output="speakers",.preamp=-2}; s.gains[0]=4;
    struct json_object *track=json("{\"players\":[{\"id\":\"test\",\"title\":\"A\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}");
    library_follow(&l,track,&s,true); library_remember(&l,&s); library_save(&l);
    /* An edit before the next player refresh must not overwrite the old song. */
    snprintf(s.output,sizeof(s.output),"headphones"); s.gains[0]=-5;
    library_remember(&l,&s);
    struct json_object *song=get(get(l.data,"songs"),"[\"speakers\",\"Artist\",\"A\"]");
    assert(json_object_get_double(json_object_array_get_idx(get(song,"gains"),0))==4);
    library_remember_output(&l,&s);
    library_select_output(&l,&s,"speakers"); assert(s.gains[0]==4 && s.preamp==-2);
    library_select_output(&l,&s,"headphones"); assert(s.gains[0]==-5);
    library_save(&l);
    s.gains[0]=7; library_remember_output(&l,&s);
    library_flush(&l,l.save_at-.1); assert(l.dirty);
    struct json_object *disk=json_object_from_file(l.path);
    assert(json_object_get_double(json_object_array_get_idx(get(get(get(disk,"output_eq"),"headphones"),"gains"),0))==-5);
    json_object_put(disk);
    library_flush(&l,l.save_at+.1); assert(!l.dirty);
    struct json_object *volumes=json("{\"speakers\":{\"volume\":73,\"mute\":true},\"headphones\":{\"volume\":0,\"mute\":false}}");
    library_volumes(&l,volumes); json_object_put(volumes);
    library_close(&l); memset(&l,0,sizeof(l)); assert(!library_open(&l,path));
    library_select_output(&l,&s,"speakers"); assert(s.gains[0]==4 && s.preamp==-2);
    assert(json_object_get_double(get(get(get(l.data,"output_volumes"),"speakers"),"volume"))==73);
    assert(json_object_get_boolean(get(get(get(l.data,"output_volumes"),"speakers"),"mute")));
    library_select_output(&l,&s,"bluez_output.AA_BB_CC_DD_EE_FF.1");
    s.gains[0]=8; library_remember_output(&l,&s);
    library_select_output(&l,&s,"speakers");
    library_select_output(&l,&s,"bluez_output.aa:bb:cc:dd:ee:ff"); assert(s.gains[0]==8);
    /* A failed song append survives successful state saves and retries once. */
    assert(!unlink(l.log_path)); assert(!mkdir(l.log_path,0700));
    library_follow(&l,track,&s,true); library_save(&l);
    assert(!*l.error && *l.log_error && json_object_array_length(l.log_queue)==1);
    library_save(&l); assert(*l.log_error && json_object_array_length(l.log_queue)==1);
    assert(!rmdir(l.log_path)); library_flush(&l,l.save_at+3);
    assert(!*l.log_error && !json_object_array_length(l.log_queue));
    char *log=slurp(l.log_path); assert(strchr(log,'\n') && !strchr(strchr(log,'\n')+1,'\n')); free(log);
    library_close(&l); json_object_put(track);
    unlink(l.path); unlink(l.log_path); rmdir(dir);
}
static void fallback_regressions(void) {
    char dir[]="/tmp/eqweb-fallback-XXXXXX",path[512]; assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/settings.json",dir);
    struct library l={0}; assert(!library_open(&l,path));
    struct settings s={.output="device.a",.preamp=-3}; s.gains[0]=1;
    struct json_object *track=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}"),
        *unknown=json("{\"players\":[{\"id\":\"test\",\"title\":\"Unfamiliar\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}");
    library_follow(&l,track,&s,true); s.gains[0]=4; library_remember(&l,&s);
    library_select_output(&l,&s,"device.b"); s.gains[0]=2; library_remember_output(&l,&s);
    library_follow(&l,track,&s,true); s.gains[0]=6; library_remember(&l,&s);
    library_select_output(&l,&s,"device.new"); s.gains[0]=9; library_remember_output(&l,&s);
    action(&l,"{\"action\":\"fallback\",\"enabled\":true}",&s);
    action(&l,"{\"action\":\"fallback_device\",\"device\":\"device.b\"}",&s);
    assert(library_follow(&l,track,&s,true) && s.gains[0]==6);
    assert(strstr(l.song_status,"device.b"));
    assert(json_object_object_length(get(l.data,"songs"))==2); /* Borrowed EQ is not an override. */
    struct json_object *snapshot=library_snapshot(&l,&s);
    assert(json_object_get_double(json_object_array_get_idx(get(get(snapshot,"device_eq"),"gains"),0))==9);
    assert(json_object_array_length(get(snapshot,"devices"))==3); json_object_put(snapshot);
    action(&l,"{\"action\":\"fallback_device\",\"device\":\"device.missing\"}",&s);
    assert(library_follow(&l,track,&s,true) && s.gains[0]==4 && strstr(l.song_status,"device.a"));
    action(&l,"{\"action\":\"fallback\",\"enabled\":false}",&s);
    assert(library_follow(&l,track,&s,true) && s.gains[0]==9);
    s.gains[0]=3; library_remember(&l,&s);
    action(&l,"{\"action\":\"fallback\",\"enabled\":true}",&s);
    assert(library_follow(&l,track,&s,true) && s.gains[0]==3); /* Own song wins. */
    assert(library_follow(&l,unknown,&s,true) && s.gains[0]==9);
    action(&l,"{\"action\":\"device_eq\",\"output\":\"device.new\",\"eq\":{\"gains\":[7,0,0,0,0,0,0,0,0,0],\"preamp\":-1}}",&s);
    assert(library_follow(&l,track,&s,true) && s.gains[0]==3);
    assert(library_follow(&l,unknown,&s,true) && s.gains[0]==7 && s.preamp==-1);
    library_close(&l); memset(&l,0,sizeof(l)); assert(!library_open(&l,path));
    s.gains[0]=-12; library_init_output(&l,&s); /* Startup must not overwrite the general EQ. */
    assert(library_follow(&l,unknown,&s,true) && s.gains[0]==7);
    assert(json_object_get_boolean(get(l.data,"fallback")));
    action(&l,"{\"action\":\"remember\",\"enabled\":false}",&s);
    library_follow(&l,track,&s,true); s.gains[0]=8; library_remember(&l,&s);
    action(&l,"{\"action\":\"remember\",\"enabled\":true}",&s);
    assert(library_follow(&l,unknown,&s,true)==false && s.gains[0]==8);
    library_close(&l); json_object_put(track); json_object_put(unknown);
    unlink(l.path); unlink(l.log_path); rmdir(dir);
}
int main(void) {
    regressions();
    fallback_regressions();
    char dir[]="/tmp/eqweb-library-XXXXXX",path[512]; assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/settings.json",dir);
    struct library l={0}; assert(!library_open(&l,path));
    struct settings s={.output="test.output"};
    struct json_object *a=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song A\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}"),
        *b=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song B\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}"),
        *other=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song A\",\"artist\":\"Someone else\",\"status\":\"Playing\"}]}");
    assert(!library_follow(&l,a,&s,true)); s.gains[0]=4; library_remember(&l,&s);
    assert(library_follow(&l,b,&s,true) && s.gains[0]==0); s.gains[0]=-3; library_remember(&l,&s);
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4 && !strcmp(s.output,"test.output"));
    assert(get(get(l.data,"presets"),"Artist - Song A (Auto)"));
    assert(get(get(l.data,"presets"),"Artist - Song B (Auto)"));
    assert(library_follow(&l,other,&s,true) && s.gains[0]==0); s.gains[0]=1; library_remember(&l,&s);
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4);
    action(&l,"{\"action\":\"save\",\"name\":\"Bass\",\"eq\":{\"gains\":[4,0,0,0,0,0,0,0,0,0],\"preamp\":-2}}",&s);
    action(&l,"{\"action\":\"quickbar\",\"eq\":{\"gains\":[6,0,0,0,0,0,0,0,0,0],\"preamp\":-1}}",&s);
    action(&l,"{\"action\":\"maintain_load\",\"enabled\":false}",&s);
    library_save(&l); assert(!l.dirty && !*l.error); library_close(&l); memset(&l,0,sizeof(l));
    assert(!library_open(&l,path)); s.gains[0]=0;
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4);
    struct json_object *snap=library_snapshot(&l,&s);
    assert(json_object_array_length(get(snap,"quickbar"))==1);
    assert(!json_object_get_boolean(get(snap,"maintain_load")));
    assert(json_object_get_boolean(get(snap,"output_log")));
    json_object_put(snap);
    action(&l,"{\"action\":\"quickbar_clear\"}",&s);
    assert(json_object_array_length(get(l.data,"quickbar"))==0);
    action(&l,"{\"action\":\"remember\",\"enabled\":false}",&s);
    assert(library_follow(&l,b,&s,true) && s.gains[0]==0);
    action(&l,"{\"action\":\"delete\",\"name\":\"Bass\"}",&s);
    library_close(&l);

    char log_path[512]; snprintf(log_path,sizeof(log_path),"%s/outputlog.txt",dir);
    char *log=slurp(log_path);
    assert(strstr(log,"Song A") && strstr(log,"Song B") && strstr(log,"\"gains\""));
    free(log);

    unlink(l.path); unlink(log_path); memset(&l,0,sizeof(l)); assert(!library_open(&l,path));
    struct settings speakers={.output="speakers"},phones={.output="headphones"};
    speakers.gains[0]=4; phones.gains[0]=-5;
    assert(!library_follow(&l,a,&speakers,true)); library_remember(&l,&speakers);
    assert(!library_follow(&l,a,&phones,true)); library_remember(&l,&phones);
    speakers.gains[0]=0; phones.gains[0]=0;
    assert(library_follow(&l,a,&speakers,true) && speakers.gains[0]==4);
    assert(library_follow(&l,a,&phones,true) && phones.gains[0]==-5);
    library_close(&l);

    char legacy_path[512]; snprintf(legacy_path,sizeof(legacy_path),"%s/library-v1.json",dir);
    FILE *f=fopen(legacy_path,"w"); assert(f);
    fputs("{\"version\":1,\"presets\":{},\"songs\":{\"[\\\"Artist\\\",\\\"Song A\\\"]\":{\"gains\":[2,0,0,0,0,0,0,0,0,0],\"preamp\":0}},\"remember\":true,\"player\":\"\"}",f);
    fclose(f);
    memset(&l,0,sizeof(l)); assert(!library_open(&l,path));
    s.gains[0]=0; s.output[0]=0; snprintf(s.output,sizeof(s.output),"test.output");
    assert(library_follow(&l,a,&s,true) && s.gains[0]==2);
    library_close(&l);

    unlink(l.path); unlink(log_path); rmdir(dir);
    json_object_put(a); json_object_put(b); json_object_put(other);
    puts("Presets, persistence, song recall, artist disambiguation, recall toggle, quickbar, per-output songs, auto presets and song log passed.");
}
