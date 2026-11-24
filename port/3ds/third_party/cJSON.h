/*
 * Minimal pure-C99 cJSON-compatible DOM parser for New 3DS Nox port.
 *
 * Implements RFC 8259 JSON parsing with recursion depth limiting so untrusted
 * or malformed lobby responses cannot overflow the 32 KB worker thread stack.
 */
#ifndef NOX_3DS_CJSON_H
#define NOX_3DS_CJSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define cJSON_Invalid (0)
#define cJSON_False   (1 << 0)
#define cJSON_True    (1 << 1)
#define cJSON_NULL    (1 << 2)
#define cJSON_Number  (1 << 3)
#define cJSON_String  (1 << 4)
#define cJSON_Array   (1 << 5)
#define cJSON_Object  (1 << 6)

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

/* Parse NUL-terminated JSON text into a DOM tree. Caller frees with cJSON_Delete(). */
cJSON *cJSON_Parse(const char *value);

/* Free a DOM tree created by cJSON_Parse(). Safe on NULL. */
void cJSON_Delete(cJSON *item);

/* Type predicates (safe on NULL). */
int cJSON_IsInvalid(const cJSON *item);
int cJSON_IsFalse(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
int cJSON_IsBool(const cJSON *item);
int cJSON_IsNull(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsObject(const cJSON *item);

/* Object / array accessors (safe on NULL). */
int cJSON_GetArraySize(const cJSON *array);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);

#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) != NULL) ? (array)->child : NULL; \
         (element) != NULL; \
         (element) = (element)->next)

#ifdef __cplusplus
}
#endif

#endif /* NOX_3DS_CJSON_H */
