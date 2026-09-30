#ifndef EQ_H
#define EQ_H
#include <stdbool.h>
#include <stddef.h>
#include <json-c/json.h>

#define BANDS 10
#define NAME_SIZE 512
#define SINK_NAME "effect_input.eqweb"
#define PLAY_NAME "effect_output.eqweb"
#define CONFIG_MARKER "# eqweb-native-v1\n"
struct settings { double gains[BANDS], preamp, loudness_target; bool maintain; char output[NAME_SIZE]; };
double average_gain(const struct settings *s);
void compensate(struct settings *s);
bool eq_same(const struct settings *a, const struct settings *b);
extern const int frequencies[BANDS];
bool valid_name(const char *name);
bool same_bluetooth_output(const char *a, const char *b);
bool parse_eq(struct json_object *o, struct settings *s);
struct json_object *settings_json(const struct settings *s);
char *render_config(const struct settings *s);
int atomic_write(const char *path, const char *data, size_t len);
int load_settings(const char *path, struct settings *s);
bool installed_config(const char *path);
int save_settings(const char *path, const struct settings *s);
int save_config(const char *path, const struct settings *s);
int backup_config(const char *path, char *backup, size_t size);
#endif
