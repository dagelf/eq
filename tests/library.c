#define _GNU_SOURCE
#include "library.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
int main(void) {
    char dir[]="/tmp/eqweb-library-XXXXXX",path[512]; assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/settings.json",dir);
    struct library l={0}; assert(!library_open(&l,path));
    struct settings s={.output="test.output"};
    struct json_object *a=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song A\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}"),
        *b=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song B\",\"artist\":\"Artist\",\"status\":\"Playing\"}]}"),
        *other=json("{\"players\":[{\"id\":\"test\",\"title\":\"Song A\",\"artist\":\"Someone else\",\"status\":\"Playing\"}]}");
    assert(!library_follow(&l,a,&s,true)); s.gains[0]=4; library_remember(&l,&s);
    assert(!library_follow(&l,b,&s,true)); s.gains[0]=-3; library_remember(&l,&s);
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4 && !strcmp(s.output,"test.output"));
    assert(get(get(l.data,"presets"),"Artist - Song A (Auto)"));
    assert(get(get(l.data,"presets"),"Artist - Song B (Auto)"));
    assert(!library_follow(&l,other,&s,true)); s.gains[0]=1; library_remember(&l,&s);
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4);
    action(&l,"{\"action\":\"save\",\"name\":\"Bass\",\"eq\":{\"gains\":[4,0,0,0,0,0,0,0,0,0],\"preamp\":-2}}",&s);
    action(&l,"{\"action\":\"quickbar\",\"eq\":{\"gains\":[6,0,0,0,0,0,0,0,0,0],\"preamp\":-1}}",&s);
    action(&l,"{\"action\":\"maintain_load\",\"enabled\":false}",&s);
    library_save(&l); assert(!l.dirty && !*l.error); library_close(&l); memset(&l,0,sizeof(l));
    assert(!library_open(&l,path)); s.gains[0]=0;
    assert(library_follow(&l,a,&s,true) && s.gains[0]==4);
    struct json_object *snap=library_snapshot(&l);
    assert(json_object_array_length(get(snap,"quickbar"))==1);
    assert(!json_object_get_boolean(get(snap,"maintain_load")));
    assert(json_object_get_boolean(get(snap,"output_log")));
    json_object_put(snap);
    action(&l,"{\"action\":\"quickbar_clear\"}",&s);
    assert(json_object_array_length(get(l.data,"quickbar"))==0);
    action(&l,"{\"action\":\"remember\",\"enabled\":false}",&s);
    assert(!library_follow(&l,b,&s,true) && s.gains[0]==4);
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
