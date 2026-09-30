#define _GNU_SOURCE
#include "eq.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    struct settings s={.output="alsa_output.test"},loaded={0};
    struct json_object *o=json_tokener_parse("{\"gains\":[0,1,2,3,4,5,6,7,8,9],\"preamp\":-6}");
    assert(parse_eq(o,&s)); json_object_put(o);
    const char *bad[]={"{}","null","{\"gains\":[0],\"preamp\":0}","{\"gains\":[0,0,0,0,0,0,0,0,0,13],\"preamp\":0}","{\"gains\":[0,0,0,0,0,0,0,0,0,0],\"preamp\":13}","{\"gains\":[0,0,0,0,0,0,0,0,0,\"1\"],\"preamp\":0}","{\"gains\":[0,0,0,0,0,0,0,0,0,NaN],\"preamp\":0}","{\"gains\":[0,0,0,0,0,0,0,0,0,0],\"preamp\":0,\"maintain\":1}"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);i++) { o=json_tokener_parse(bad[i]); assert(!parse_eq(o,&s)); if (o) json_object_put(o); }
    assert(!valid_name(SINK_NAME)); assert(!valid_name("evil\" }")); assert(!valid_name(""));
    assert(same_bluetooth_output("bluez_output.E4_58_BC_C4_DA_17.1","bluez_output.e4:58:bc:c4:da:17"));
    assert(same_bluetooth_output("bluez_output.E4_58_BC_C4_DA_17.1","bluez_output.E4_58_BC_C4_DA_17.2"));
    assert(!same_bluetooth_output("bluez_output.E4_58_BC_C4_DA_17.1","bluez_output.E4:58:BC:C4:DA:18"));
    assert(!same_bluetooth_output("bluez_output.E4_58_BC_C4_DA_17.1","bluez_input.E4:58:BC:C4:DA:17"));
    assert(!same_bluetooth_output("bluez_output.short","bluez_output.short"));
    assert(!same_bluetooth_output("alsa_output.test","alsa_output.test"));
    char *config=render_config(&s); assert(config);
    assert(strstr(config,"audio.channels = 2")); assert(strstr(config,"node.dont-fallback = true"));
    assert(strstr(config,"name = band10 label = bq_peaking")); assert(strstr(config,"Q = 1.4"));
    assert(strstr(config,"target.object = \"alsa_output.test\"")); free(config);
    char dir[]="/tmp/eqweb-unit-XXXXXX"; assert(mkdtemp(dir));
    char path[512],backup[600]; snprintf(path,sizeof(path),"%s/sub/state.json",dir);
    assert(save_settings(path,&s)==0); assert(load_settings(path,&loaded)==1);
    assert(loaded.gains[9]==9 && loaded.preamp==-6 && !strcmp(loaded.output,s.output));
    assert(backup_config(path,backup,sizeof(backup))==1);
    assert(save_config(path,&s)==0 && installed_config(path));
    assert(load_settings(backup,&loaded)==1);
    assert(atomic_write(path,"broken",6)==0); assert(load_settings(path,&loaded)==-1 && errno==EINVAL);
    assert(save_settings("/proc/eqweb-impossible/state.json",&s)==-1);
    o=json_tokener_parse("{\"gains\":[6,6,6,6,6,6,6,6,6,6],\"preamp\":0,\"maintain\":true,\"loudness_target\":0}");
    assert(parse_eq(o,&s)); json_object_put(o); assert(average_gain(&s)==6 && s.preamp==-6);
    for (int i=0;i<BANDS;i++) s.gains[i]=-6;
    compensate(&s); assert(s.preamp==6 && fabs(s.preamp+average_gain(&s))<.0001);
    s.loudness_target=-36; compensate(&s); assert(s.preamp==-24);
    assert(save_settings(path,&s)==0 && load_settings(path,&loaded)==1 && loaded.maintain && loaded.loudness_target==-36);
    assert(eq_same(&s,&loaded) && !eq_same(&s,&(struct settings){0}));
    unlink(path); unlink(backup); snprintf(path,sizeof(path),"%s/sub",dir); rmdir(path); rmdir(dir);
    puts("Configuration, validation, atomic persistence, backup and failure tests passed.");
}
