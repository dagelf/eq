/* Measure sample delay between the EQ input monitor and selected output monitor. */
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdio.h>
#include <stdlib.h>
#define FRAMES 96000
static struct pw_main_loop *loop;
static struct spa_source *done;
static void *reference,*processed;
static float samples[FRAMES][2];
static unsigned count;
static bool started;
static void finish(void *data,uint64_t v) { (void)data; (void)v; pw_main_loop_quit(loop); }
static void process(void *data,struct spa_io_position *position) {
    (void)data;
    unsigned n=position->clock.duration;
    float *a=pw_filter_get_dsp_buffer(reference,n),*b=pw_filter_get_dsp_buffer(processed,n);
    if (!a || !b || count==FRAMES) return;
    for (unsigned i=0;i<n && count<FRAMES;i++) {
        if (fabsf(a[i])>.0001f) started=true;
        if (!started) continue;
        samples[count][0]=a[i]; samples[count++][1]=b[i];
    }
    if (count==FRAMES) pw_loop_signal_event(pw_main_loop_get_loop(loop),done);
}
static const struct pw_filter_events events={PW_VERSION_FILTER_EVENTS,.process=process};
int main(void) {
    pw_init(NULL,NULL); loop=pw_main_loop_new(NULL); if (!loop) return 1;
    done=pw_loop_add_event(pw_main_loop_get_loop(loop),finish,NULL);
    struct spa_source *timer=pw_loop_add_timer(pw_main_loop_get_loop(loop),finish,NULL);
    struct timespec timeout={10,0}; pw_loop_update_timer(pw_main_loop_get_loop(loop),timer,&timeout,NULL,false);
    struct pw_filter *filter=pw_filter_new_simple(pw_main_loop_get_loop(loop),"EQ latency measurement",
        pw_properties_new(PW_KEY_NODE_NAME,"eqweb-latency",PW_KEY_MEDIA_TYPE,"Audio",PW_KEY_MEDIA_CATEGORY,"Capture",PW_KEY_MEDIA_ROLE,"DSP",PW_KEY_NODE_AUTOCONNECT,"false",NULL),&events,NULL);
    if (!filter) return 1;
    reference=pw_filter_add_port(filter,PW_DIRECTION_INPUT,PW_FILTER_PORT_FLAG_MAP_BUFFERS,0,
        pw_properties_new(PW_KEY_FORMAT_DSP,"32 bit float mono audio",PW_KEY_PORT_NAME,"reference",NULL),NULL,0);
    processed=pw_filter_add_port(filter,PW_DIRECTION_INPUT,PW_FILTER_PORT_FLAG_MAP_BUFFERS,0,
        pw_properties_new(PW_KEY_FORMAT_DSP,"32 bit float mono audio",PW_KEY_PORT_NAME,"processed",NULL),NULL,0);
    if (!reference || !processed || pw_filter_connect(filter,PW_FILTER_FLAG_RT_PROCESS,NULL,0)<0) return 1;
    pw_main_loop_run(loop);
    pw_filter_destroy(filter); pw_loop_destroy_source(pw_main_loop_get_loop(loop),done); pw_loop_destroy_source(pw_main_loop_get_loop(loop),timer); pw_main_loop_destroy(loop); pw_deinit();
    if (count<FRAMES) { fprintf(stderr,"Latency: only %u frames received\n",count); return 1; }
    double best=-1; int delay=0;
    for (int lag=-1024;lag<=1024;lag++) {
        double xy=0,xx=0,yy=0;
        for (int i=8192;i<32768;i++) { double x=samples[i][0],y=samples[i+lag][1]; xy+=x*y; xx+=x*x; yy+=y*y; }
        double correlation=xy/sqrt(xx*yy);
        if (correlation>best) { best=correlation; delay=lag; }
    }
    printf("EQ input monitor to output monitor: %d frames (%.3f ms at 48 kHz), correlation %.8f\n",delay,delay/48.0,best);
    return best>.999?0:1;
}
