#ifndef DESKTOP_H
#define DESKTOP_H
#include <json-c/json.h>
int desktop_start(void);
void desktop_stop(void);
struct json_object *desktop_snapshot(void);
/* Validates and queues a mixer or player action; NULL means accepted. */
const char *desktop_action(const char *kind,struct json_object *body);
#endif
