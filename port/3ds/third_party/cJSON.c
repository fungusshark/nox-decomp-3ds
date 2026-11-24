/*
 * Minimal pure-C99 cJSON-compatible DOM parser for New 3DS Nox port.
 */
#include "cJSON.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CJSON_MAX_DEPTH 32

typedef struct {
    const char *src;
    size_t pos;
    int depth;
} parse_ctx_t;

static cJSON *cjson_new_item(void)
{
    return (cJSON *)calloc(1, sizeof(cJSON));
}

void cJSON_Delete(cJSON *item)
{
    while (item) {
        cJSON *next = item->next;
        if (item->child)
            cJSON_Delete(item->child);
        free(item->valuestring);
        free(item->string);
        free(item);
        item = next;
    }
}

static void skip_ws(parse_ctx_t *ctx)
{
    while (ctx->src[ctx->pos] != '\0') {
        unsigned char c = (unsigned char)ctx->src[ctx->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            ctx->pos++;
        else
            break;
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_hex4(const char *s, unsigned int *out)
{
    unsigned int v = 0;
    for (int i = 0; i < 4; ++i) {
        int d = hex_val(s[i]);
        if (d < 0) return 0;
        v = (v << 4) | (unsigned int)d;
    }
    *out = v;
    return 1;
}

static size_t encode_utf8(unsigned int cp, char *dst)
{
    if (cp <= 0x7Fu) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FFu) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp <= 0xFFFFu) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        dst[0] = (char)(0xF0u | (cp >> 18));
        dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

static char *parse_string_raw(parse_ctx_t *ctx)
{
    if (ctx->src[ctx->pos] != '"')
        return NULL;
    ctx->pos++; /* opening quote */

    /* Upper bound on decoded length: raw length between quotes + 1 */
    size_t start = ctx->pos;
    size_t scan = start;
    while (ctx->src[scan] != '\0' && ctx->src[scan] != '"') {
        if (ctx->src[scan] == '\\') {
            if (ctx->src[scan + 1] == '\0')
                return NULL;
            scan += 2;
        } else {
            if ((unsigned char)ctx->src[scan] < 0x20u)
                return NULL;
            scan++;
        }
    }
    if (ctx->src[scan] != '"')
        return NULL;

    size_t raw_len = scan - start;
    char *out = (char *)malloc(raw_len + 1u);
    if (!out)
        return NULL;

    size_t out_len = 0;
    size_t i = start;
    while (i < scan) {
        char c = ctx->src[i++];
        if (c != '\\') {
            out[out_len++] = c;
            continue;
        }
        if (i >= scan) {
            free(out);
            return NULL;
        }
        char esc = ctx->src[i++];
        switch (esc) {
        case '"':  out[out_len++] = '"';  break;
        case '\\': out[out_len++] = '\\'; break;
        case '/':  out[out_len++] = '/';  break;
        case 'b':  out[out_len++] = '\b'; break;
        case 'f':  out[out_len++] = '\f'; break;
        case 'n':  out[out_len++] = '\n'; break;
        case 'r':  out[out_len++] = '\r'; break;
        case 't':  out[out_len++] = '\t'; break;
        case 'u': {
            if (i + 4u > scan) {
                free(out);
                return NULL;
            }
            unsigned int cp = 0;
            if (!parse_hex4(&ctx->src[i], &cp)) {
                free(out);
                return NULL;
            }
            i += 4u;
            /* Handle UTF-16 surrogate pair \uD800..\uDBFF + \uDC00..\uDFFF */
            if (cp >= 0xD800u && cp <= 0xDBFFu &&
                i + 6u <= scan && ctx->src[i] == '\\' && ctx->src[i + 1] == 'u') {
                unsigned int low = 0;
                if (parse_hex4(&ctx->src[i + 2], &low) &&
                    low >= 0xDC00u && low <= 0xDFFFu) {
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    i += 6u;
                }
            }
            out_len += encode_utf8(cp, &out[out_len]);
            break;
        }
        default:
            free(out);
            return NULL;
        }
    }

    out[out_len] = '\0';
    ctx->pos = scan + 1u; /* past closing quote */
    return out;
}

static cJSON *parse_value(parse_ctx_t *ctx);

static cJSON *parse_number(parse_ctx_t *ctx)
{
    const char *start = &ctx->src[ctx->pos];
    char *endp = NULL;
    double val = strtod(start, &endp);
    if (endp == start || endp == NULL)
        return NULL;

    cJSON *item = cjson_new_item();
    if (!item)
        return NULL;

    item->type = cJSON_Number;
    item->valuedouble = val;
    if (val >= (double)INT_MAX)
        item->valueint = INT_MAX;
    else if (val <= (double)INT_MIN)
        item->valueint = INT_MIN;
    else
        item->valueint = (int)val;

    ctx->pos = (size_t)(endp - ctx->src);
    return item;
}

static cJSON *parse_array(parse_ctx_t *ctx)
{
    if (ctx->depth >= CJSON_MAX_DEPTH)
        return NULL;

    ctx->pos++; /* '[' */
    ctx->depth++;

    cJSON *arr = cjson_new_item();
    if (!arr) {
        ctx->depth--;
        return NULL;
    }
    arr->type = cJSON_Array;

    skip_ws(ctx);
    if (ctx->src[ctx->pos] == ']') {
        ctx->pos++;
        ctx->depth--;
        return arr;
    }

    cJSON *tail = NULL;
    for (;;) {
        skip_ws(ctx);
        cJSON *elem = parse_value(ctx);
        if (!elem) {
            cJSON_Delete(arr);
            ctx->depth--;
            return NULL;
        }
        if (!arr->child) {
            arr->child = elem;
        } else {
            tail->next = elem;
            elem->prev = tail;
        }
        tail = elem;

        skip_ws(ctx);
        if (ctx->src[ctx->pos] == ',') {
            ctx->pos++;
            continue;
        }
        if (ctx->src[ctx->pos] == ']') {
            ctx->pos++;
            break;
        }
        cJSON_Delete(arr);
        ctx->depth--;
        return NULL;
    }

    ctx->depth--;
    return arr;
}

static cJSON *parse_object(parse_ctx_t *ctx)
{
    if (ctx->depth >= CJSON_MAX_DEPTH)
        return NULL;

    ctx->pos++; /* '{' */
    ctx->depth++;

    cJSON *obj = cjson_new_item();
    if (!obj) {
        ctx->depth--;
        return NULL;
    }
    obj->type = cJSON_Object;

    skip_ws(ctx);
    if (ctx->src[ctx->pos] == '}') {
        ctx->pos++;
        ctx->depth--;
        return obj;
    }

    cJSON *tail = NULL;
    for (;;) {
        skip_ws(ctx);
        char *key = parse_string_raw(ctx);
        if (!key) {
            cJSON_Delete(obj);
            ctx->depth--;
            return NULL;
        }

        skip_ws(ctx);
        if (ctx->src[ctx->pos] != ':') {
            free(key);
            cJSON_Delete(obj);
            ctx->depth--;
            return NULL;
        }
        ctx->pos++; /* ':' */

        skip_ws(ctx);
        cJSON *val = parse_value(ctx);
        if (!val) {
            free(key);
            cJSON_Delete(obj);
            ctx->depth--;
            return NULL;
        }
        val->string = key;

        if (!obj->child) {
            obj->child = val;
        } else {
            tail->next = val;
            val->prev = tail;
        }
        tail = val;

        skip_ws(ctx);
        if (ctx->src[ctx->pos] == ',') {
            ctx->pos++;
            continue;
        }
        if (ctx->src[ctx->pos] == '}') {
            ctx->pos++;
            break;
        }
        cJSON_Delete(obj);
        ctx->depth--;
        return NULL;
    }

    ctx->depth--;
    return obj;
}

static cJSON *parse_value(parse_ctx_t *ctx)
{
    skip_ws(ctx);
    const char *p = &ctx->src[ctx->pos];
    if (*p == '\0')
        return NULL;

    if (*p == '{')
        return parse_object(ctx);
    if (*p == '[')
        return parse_array(ctx);
    if (*p == '"') {
        char *s = parse_string_raw(ctx);
        if (!s) return NULL;
        cJSON *item = cjson_new_item();
        if (!item) {
            free(s);
            return NULL;
        }
        item->type = cJSON_String;
        item->valuestring = s;
        return item;
    }
    if (strncmp(p, "true", 4) == 0) {
        cJSON *item = cjson_new_item();
        if (!item) return NULL;
        item->type = cJSON_True;
        item->valueint = 1;
        item->valuedouble = 1.0;
        ctx->pos += 4u;
        return item;
    }
    if (strncmp(p, "false", 5) == 0) {
        cJSON *item = cjson_new_item();
        if (!item) return NULL;
        item->type = cJSON_False;
        ctx->pos += 5u;
        return item;
    }
    if (strncmp(p, "null", 4) == 0) {
        cJSON *item = cjson_new_item();
        if (!item) return NULL;
        item->type = cJSON_NULL;
        ctx->pos += 4u;
        return item;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return parse_number(ctx);

    return NULL;
}

cJSON *cJSON_Parse(const char *value)
{
    if (!value)
        return NULL;

    parse_ctx_t ctx;
    ctx.src = value;
    ctx.pos = 0;
    ctx.depth = 0;

    /* Skip optional UTF-8 BOM */
    if ((unsigned char)value[0] == 0xEFu &&
        (unsigned char)value[1] == 0xBBu &&
        (unsigned char)value[2] == 0xBFu) {
        ctx.pos = 3u;
    }

    cJSON *root = parse_value(&ctx);
    if (!root)
        return NULL;

    skip_ws(&ctx);
    if (ctx.src[ctx.pos] != '\0') {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

int cJSON_IsInvalid(const cJSON *item) { return item == NULL || item->type == cJSON_Invalid; }
int cJSON_IsFalse(const cJSON *item)   { return item != NULL && item->type == cJSON_False; }
int cJSON_IsTrue(const cJSON *item)    { return item != NULL && item->type == cJSON_True; }
int cJSON_IsBool(const cJSON *item)    { return item != NULL && (item->type == cJSON_True || item->type == cJSON_False); }
int cJSON_IsNull(const cJSON *item)    { return item != NULL && item->type == cJSON_NULL; }
int cJSON_IsNumber(const cJSON *item)  { return item != NULL && item->type == cJSON_Number; }
int cJSON_IsString(const cJSON *item)  { return item != NULL && item->type == cJSON_String && item->valuestring != NULL; }
int cJSON_IsArray(const cJSON *item)   { return item != NULL && item->type == cJSON_Array; }
int cJSON_IsObject(const cJSON *item)  { return item != NULL && item->type == cJSON_Object; }

int cJSON_GetArraySize(const cJSON *array)
{
    if (!array) return 0;
    int count = 0;
    for (const cJSON *c = array->child; c != NULL; c = c->next)
        count++;
    return count;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (!array || index < 0) return NULL;
    cJSON *c = array->child;
    while (c && index > 0) {
        c = c->next;
        index--;
    }
    return c;
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    if (!object || !string) return NULL;
    for (cJSON *c = object->child; c != NULL; c = c->next) {
        if (c->string && strcmp(c->string, string) == 0)
            return c;
    }
    return NULL;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    if (!object || !string) return NULL;
    for (cJSON *c = object->child; c != NULL; c = c->next) {
        if (!c->string) continue;
        const char *a = c->string;
        const char *b = string;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
            return c;
    }
    return NULL;
}
