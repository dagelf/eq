#ifndef LIBRARY_H
#define LIBRARY_H
#include "eq.h"
struct library {
    struct json_object *data;
    char path[4096], log_path[4096], error[512], song[2048], song_status[256];
    bool dirty, baseline_valid;
    struct settings baseline;
};
int library_open(struct library *l,const char *state_path);
void library_save(struct library *l);
void library_close(struct library *l);
struct json_object *library_snapshot(struct library *l);
const char *library_action(struct library *l,struct json_object *body,const struct settings *current);
void library_remember(struct library *l,const struct settings *current);
bool library_follow(struct library *l,struct json_object *desktop,struct settings *current,bool editing);
#endif
