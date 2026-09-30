#ifndef DESKTOP_H
#define DESKTOP_H
#include <json-c/json.h>
int desktop_start(struct json_object *output_volumes);
/* Stops the worker after accepted commands; caller owns the final volume map. */
struct json_object *desktop_stop(void);
const char *desktop_select_output(const char *output,unsigned *epoch);
struct json_object *desktop_snapshot(void);
/* Validates and queues a mixer or player action; NULL means accepted. */
const char *desktop_action(const char *kind,struct json_object *body);
#endif
