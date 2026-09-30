#ifndef JSON_UTIL_H
#define JSON_UTIL_H
#include <json-c/json.h>

static inline struct json_object *json_get(struct json_object *o, const char *key) {
    struct json_object *value=NULL;
    json_object_object_get_ex(o,key,&value);
    return value;
}
static inline const char *json_string(struct json_object *o, const char *key) {
    struct json_object *value=json_get(o,key);
    return json_object_is_type(value,json_type_string)?json_object_get_string(value):"";
}
static inline struct json_object *json_copy(struct json_object *o) {
    struct json_object *copy=NULL;
    if (o) json_object_deep_copy(o,&copy,NULL);
    return copy;
}
#endif
