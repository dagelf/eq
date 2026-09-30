#define _GNU_SOURCE
#include "eq.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const int frequencies[BANDS] = {31,63,125,250,500,1000,2000,4000,8000,16000};

bool valid_name(const char *s) {
    if (!s || !*s || strlen(s) >= NAME_SIZE || !strcmp(s,SINK_NAME) || !strcmp(s,PLAY_NAME)) return false;
    for (const unsigned char *p=(const unsigned char *)s; *p; p++)
        if (!isalnum(*p) && !strchr("._:-",*p)) return false;
    return true;
}
/* BlueZ has used both MAC separators and endpoint suffixes in output names.
 * Compare the device address, never a human-readable (non-unique) label. */
static bool bluetooth_address(const char *name,char address[18]) {
    const char *prefix="bluez_output.";
    if (!valid_name(name) || strncmp(name,prefix,strlen(prefix))) return false;
    const char *p=name+strlen(prefix);
    if (strlen(p)<17 || (p[17] && p[17]!='.')) return false;
    for (int i=0;i<17;i++) {
        if (i%3==2) {
            if (p[i]!=':' && p[i]!='_') return false;
            address[i]=':';
        } else {
            if (!isxdigit((unsigned char)p[i])) return false;
            address[i]=tolower((unsigned char)p[i]);
        }
    }
    address[17]=0; return true;
}
bool same_bluetooth_output(const char *a,const char *b) {
    char x[18],y[18];
    return bluetooth_address(a,x) && bluetooth_address(b,y) && !strcmp(x,y);
}
void output_key(const char *name,char key[NAME_SIZE]) {
    char address[18];
    if (bluetooth_address(name,address)) snprintf(key,NAME_SIZE,"bluez_output.%s",address);
    else snprintf(key,NAME_SIZE,"%s",name);
}
static bool number(struct json_object *o, double lo, double hi, double *v) {
    if (!o || !(json_object_is_type(o,json_type_int) || json_object_is_type(o,json_type_double))) return false;
    *v=json_object_get_double(o);
    return isfinite(*v) && *v>=lo && *v<=hi;
}
double average_gain(const struct settings *s) {
    double sum=0; for (int i=0;i<BANDS;i++) sum+=s->gains[i]; return sum/BANDS;
}
void compensate(struct settings *s) {
    if (s->maintain) s->preamp=fmax(-24,fmin(12,s->loudness_target-average_gain(s)));
}
bool eq_same(const struct settings *a, const struct settings *b) {
    if (fabs(a->preamp-b->preamp)>0.05) return false;
    for (int i=0;i<BANDS;i++) if (fabs(a->gains[i]-b->gains[i])>0.05) return false;
    return true;
}
bool parse_eq(struct json_object *o, struct settings *s) {
    struct json_object *g,*p;
    struct settings n=*s;
    if (!json_object_is_type(o,json_type_object) ||
        !json_object_object_get_ex(o,"gains",&g) || !json_object_is_type(g,json_type_array) ||
        json_object_array_length(g)!=BANDS || !json_object_object_get_ex(o,"preamp",&p) ||
        !number(p,-24,12,&n.preamp)) return false;
    for (int i=0;i<BANDS;i++) if (!number(json_object_array_get_idx(g,i),-12,12,&n.gains[i])) return false;
    struct json_object *v;
    if (json_object_object_get_ex(o,"maintain",&v)) {
        if (!json_object_is_type(v,json_type_boolean)) return false;
        n.maintain=json_object_get_boolean(v);
    }
    if (json_object_object_get_ex(o,"loudness_target",&v)) {
        if (!number(v,-36,24,&n.loudness_target)) return false;
    } else n.loudness_target=average_gain(&n)+n.preamp;
    compensate(&n);
    *s=n;
    return true;
}
struct json_object *settings_json(const struct settings *s) {
    struct json_object *o=json_object_new_object(), *g=json_object_new_array();
    for (int i=0;i<BANDS;i++) json_object_array_add(g,json_object_new_double(s->gains[i]));
    json_object_object_add(o,"gains",g);
    json_object_object_add(o,"preamp",json_object_new_double(s->preamp));
    json_object_object_add(o,"maintain",json_object_new_boolean(s->maintain));
    json_object_object_add(o,"loudness_target",json_object_new_double(s->loudness_target));
    json_object_object_add(o,"output",json_object_new_string(s->output));
    json_object_object_add(o,"version",json_object_new_int(1));
    return o;
}
char *render_config(const struct settings *s) {
    char *buf=NULL; size_t len=0;
    FILE *f=open_memstream(&buf,&len);
    if (!f) return NULL;
    fprintf(f,CONFIG_MARKER "context.modules = [\n { name = libpipewire-module-filter-chain\n args = {\n"
        " node.description = \"EQ Web Sink\"\n media.name = \"EQ Web Sink\"\n filter.graph = {\n nodes = [\n"
        " { type = builtin name = preamp label = linear control = { Mult = %.9g Add = 0.0 } }\n",pow(10,s->preamp/20));
    for (int i=0;i<BANDS;i++) fprintf(f," { type = builtin name = band%d label = bq_peaking control = { Freq = %d.0 Q = 1.4 Gain = %.9g } }\n",i+1,frequencies[i],s->gains[i]);
    fputs(" ]\n links = [\n { output = \"preamp:Out\" input = \"band1:In\" }\n",f);
    for (int i=1;i<BANDS;i++) fprintf(f," { output = \"band%d:Out\" input = \"band%d:In\" }\n",i,i+1);
    fputs(" ]\n inputs = [ \"preamp:In\" ]\n outputs = [ \"band10:Out\" ]\n }\n"
        " audio.channels = 2\n audio.position = [ FL FR ]\n"
        " capture.props = { node.name = \"" SINK_NAME "\" media.class = Audio/Sink eqweb.version = 1 node.virtual = true priority.session = 0 }\n",f);
    fprintf(f," playback.props = { node.name = \"" PLAY_NAME "\" node.passive = true node.autoconnect = true\n"
        " node.dont-fallback = true node.linger = true node.dont-reconnect = false\n"
        " node.stream.restore-target = false node.stream.restore-props = false target.object = \"%s\" }\n }\n }\n]\n",s->output);
    if (fclose(f)) { free(buf); return NULL; }
    return buf;
}
static int make_parents(char *path) {
    for (char *p=path+1;*p;p++) if (*p=='/') {
        *p=0; int r=mkdir(path,0700); *p='/';
        if (r && errno!=EEXIST) return -1;
    }
    return 0;
}
int atomic_write(const char *path, const char *data, size_t len) {
    char *tmp=NULL,*parent=strdup(path); int fd=-1,dir=-1,r=-1,saved;
    if (!parent || asprintf(&tmp,"%s.tmp.XXXXXX",path)<0) goto out;
    if (make_parents(parent)) goto out;
    char *slash=strrchr(parent,'/');
    if (slash) { if (slash==parent) slash[1]=0; else *slash=0; } else strcpy(parent,".");
    dir=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (dir<0 || (fd=mkstemp(tmp))<0) goto out;
    for (size_t n=0;n<len;) {
        ssize_t w=write(fd,data+n,len-n);
        if (w<0 && errno==EINTR) continue;
        if (w<=0) goto out;
        n+=(size_t)w;
    }
    if (fsync(fd)) goto out;
    if (close(fd)) { fd=-1; goto out; } fd=-1;
    if (rename(tmp,path) || fsync(dir)) goto out;
    r=0;
out:
    saved=errno;
    if (fd>=0) close(fd);
    if (r && tmp) unlink(tmp);
    if (dir>=0) close(dir);
    free(tmp); free(parent); errno=saved; return r;
}
int load_settings(const char *path, struct settings *s) {
    if (access(path,F_OK)) return errno==ENOENT ? 0 : -1;
    struct json_object *o=json_object_from_file(path), *output,*version;
    struct settings n=*s;
    bool ok=o && parse_eq(o,&n) && json_object_object_get_ex(o,"version",&version) &&
        json_object_get_int(version)==1 && json_object_object_get_ex(o,"output",&output) &&
        json_object_is_type(output,json_type_string) && valid_name(json_object_get_string(output)) &&
        strlen(json_object_get_string(output))==(size_t)json_object_get_string_len(output);
    if (ok) { snprintf(n.output,sizeof(n.output),"%s",json_object_get_string(output)); *s=n; }
    if (o) json_object_put(o);
    if (!ok) { errno=EINVAL; return -1; }
    return 1;
}
bool installed_config(const char *path) {
    FILE *f=fopen(path,"r"); char line[128];
    if (!f) return false;
    bool ok=fgets(line,sizeof(line),f) && !strcmp(line,CONFIG_MARKER);
    fclose(f); return ok;
}
int save_settings(const char *path, const struct settings *s) {
    struct json_object *o=settings_json(s);
    const char *buf=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PRETTY);
    int r=atomic_write(path,buf,strlen(buf)); json_object_put(o); return r;
}
int save_config(const char *path, const struct settings *s) {
    char *buf=render_config(s); if (!buf) return -1;
    int r=atomic_write(path,buf,strlen(buf)); free(buf); return r;
}
int backup_config(const char *path, char *backup, size_t size) {
    FILE *f=fopen(path,"rb");
    if (!f) return errno==ENOENT ? 0 : -1;
    if (snprintf(backup,size,"%s.backup.XXXXXX",path)>=(int)size) { fclose(f); errno=ENAMETOOLONG; return -1; }
    int fd=mkstemp(backup),r=0; char buf[4096]; size_t n;
    if (fd<0) { fclose(f); return -1; }
    while ((n=fread(buf,1,sizeof(buf),f))) {
        size_t off=0;
        while (off<n) { ssize_t w=write(fd,buf+off,n-off); if (w<0 && errno==EINTR) continue; if (w<=0) { r=-1; break; } off+=(size_t)w; }
        if (r) break;
    }
    if (ferror(f) || fsync(fd)) r=-1;
    if (close(fd)) r=-1;
    fclose(f); return r ? -1 : 1;
}
