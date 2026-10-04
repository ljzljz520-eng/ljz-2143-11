#ifndef STUB_JSON_H
#define STUB_JSON_H
#include <stdint.h>
typedef struct json_object json_object;
json_object *json_object_new_object(void);
json_object *json_object_new_array(void);
json_object *json_object_new_string(const char*);
json_object *json_object_new_boolean(int);
json_object *json_object_new_int64(int64_t);
void json_object_object_add(json_object*,const char*,json_object*);
void json_object_array_add(json_object*,json_object*);
int json_object_object_get_ex(json_object*,const char*,json_object**);
int json_object_is_null(json_object*);
const char *json_object_get_string(json_object*);
int json_object_get_boolean(json_object*);
int64_t json_object_get_int64(json_object*);
json_object *json_tokener_parse(const char*);
void json_object_put(json_object*);
const char *json_object_to_json_string_ext(json_object*,int);
#define JSON_C_TO_STRING_PLAIN 0
#endif
