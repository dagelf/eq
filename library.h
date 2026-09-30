#ifndef LIBRARY_H
#define LIBRARY_H
#include "eq.h"
struct library {
    struct json_object *data, *log_queue, *devices;
    char path[4096], log_path[4096], error[512], log_error[512], song[2048], song_status[256], song_output[NAME_SIZE];
    double save_at, retry_at;
    bool dirty, baseline_valid, general_pending;
    struct settings baseline;
};
int library_open(struct library *l,const char *state_path);
void library_save(struct library *l);
void library_flush(struct library *l, double now);
void library_close(struct library *l);
struct json_object *library_snapshot(struct library *l,const struct settings *current);
const char *library_action(struct library *l,struct json_object *body,const struct settings *current);
void library_remember(struct library *l,const struct settings *current);
void library_remember_output(struct library *l,const struct settings *current);
void library_init_output(struct library *l,const struct settings *current);
void library_select_output(struct library *l,struct settings *current,const char *output);
void library_volumes(struct library *l,struct json_object *volumes);
bool library_follow(struct library *l,struct json_object *desktop,struct settings *current,bool editing);
#endif
