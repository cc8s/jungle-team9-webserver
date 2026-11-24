#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSON_DEPTH 16
#define JSON_MEMBERS 100
#define JSON_ELEMENTS 1000
#define JSON_VALUES 10000

typedef struct {
    const unsigned char *data;
    size_t length, at, values;
    int error;
} parser;

int json_append(json_output *out, const char *text, size_t length)
{
    if (length > SIZE_MAX - out->length - 1)
        return 500;
    size_t needed = out->length + length + 1;
    if (needed > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 128;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) {
                capacity = needed;
                break;
            }
            capacity *= 2;
        }
        char *grown = realloc(out->data, capacity);
        if (!grown)
            return 500;
        out->data = grown;
        out->capacity = capacity;
    }
    if (length)
        memcpy(out->data + out->length, text, length);
    out->length += length;
    out->data[out->length] = 0;
    return 0;
}

static void space(parser *p)
{
    while (p->at < p->length && strchr(" \t\r\n", p->data[p->at]) && p->data[p->at])
        p->at++;
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Return UTF-8 character width, rejecting overlongs, surrogates and > U+10FFFF. */
static size_t utf8_width(const unsigned char *data, size_t length)
{
    if (!length)
        return 0;
    unsigned c = data[0];
    if (c < 128)
        return 1;
    size_t n = c >= 0xc2 && c <= 0xdf   ? 2
               : c >= 0xe0 && c <= 0xef ? 3
               : c >= 0xf0 && c <= 0xf4 ? 4
                                        : 0;
    if (!n || length < n)
        return 0;
    for (size_t i = 1; i < n; i++)
        if ((data[i] & 0xc0) != 0x80)
            return 0;
    if ((c == 0xe0 && data[1] < 0xa0) || (c == 0xed && data[1] >= 0xa0) ||
        (c == 0xf0 && data[1] < 0x90) || (c == 0xf4 && data[1] >= 0x90))
        return 0;
    return n;
}

static int codepoint(json_output *out, unsigned cp)
{
    unsigned char bytes[4];
    size_t n;
    if (cp < 0x80) {
        bytes[0] = cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = 0xc0 | (cp >> 6);
        bytes[1] = 0x80 | (cp & 63);
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = 0xe0 | (cp >> 12);
        bytes[1] = 0x80 | ((cp >> 6) & 63);
        bytes[2] = 0x80 | (cp & 63);
        n = 3;
    } else {
        bytes[0] = 0xf0 | (cp >> 18);
        bytes[1] = 0x80 | ((cp >> 12) & 63);
        bytes[2] = 0x80 | ((cp >> 6) & 63);
        bytes[3] = 0x80 | (cp & 63);
        n = 4;
    }
    return json_append(out, (char *)bytes, n);
}

static int unicode4(parser *p, unsigned *cp)
{
    if (p->length - p->at < 4)
        return 400;
    *cp = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex(p->data[p->at++]);
        if (h < 0)
            return 400;
        *cp = (*cp << 4) | (unsigned)h;
    }
    return 0;
}

static int string(parser *p, json_bytes *text)
{
    json_output out = {0};
    int error = 400;
    if (p->at >= p->length || p->data[p->at++] != '"')
        return 400;
    while (p->at < p->length) {
        unsigned char c = p->data[p->at++];
        if (c == '"') {
            if (!out.data && json_append(&out, "", 0)) {
                error = 500;
                break;
            }
            text->data = (unsigned char *)out.data;
            text->length = out.length;
            return 0;
        }
        if (c < 32)
            break;
        if (c == '\\') {
            if (p->at == p->length)
                break;
            c = p->data[p->at++];
            if (c == 'u') {
                unsigned cp;
                if (unicode4(p, &cp))
                    break;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (p->length - p->at < 6 || p->data[p->at] != '\\' ||
                        p->data[p->at + 1] != 'u')
                        break;
                    p->at += 2;
                    unsigned low;
                    if (unicode4(p, &low) || low < 0xdc00 || low > 0xdfff)
                        break;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff)
                    break;
                if (codepoint(&out, cp)) {
                    error = 500;
                    break;
                }
                continue;
            }
            switch (c) {
            case '"':
            case '\\':
            case '/':
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            default:
                goto done;
            }
        } else if (c >= 128) {
            size_t start = p->at - 1, n = utf8_width(p->data + start, p->length - start);
            if (!n)
                break;
            if (json_append(&out, (const char *)p->data + start, n)) {
                error = 500;
                break;
            }
            p->at = start + n;
            continue;
        }
        if (json_append(&out, (const char *)&c, 1)) {
            error = 500;
            break;
        }
    }
done:
    free(out.data);
    return error;
}

void json_free(json_value *value)
{
    while (value) {
        json_value *next = value->next;
        json_free(value->child);
        free(value->text.data);
        free(value->key.data);
        free(value);
        value = next;
    }
}

static int same(json_bytes a, json_bytes b)
{
    return a.length == b.length && (!a.length || !memcmp(a.data, b.data, a.length));
}

static json_value *value(parser *p, size_t depth);
static int container(parser *p, json_value *v, size_t depth)
{
    if (++depth > JSON_DEPTH)
        return 413;
    int object = p->data[p->at++] == '{';
    unsigned char end = object ? '}' : ']';
    v->type = object ? JSON_OBJECT : JSON_ARRAY;
    space(p);
    if (p->at < p->length && p->data[p->at] == end) {
        p->at++;
        return 0;
    }
    size_t count = 0;
    json_value **tail = &v->child;
    for (;;) {
        if (++count > (object ? JSON_MEMBERS : JSON_ELEMENTS))
            return 413;
        json_bytes key = {0};
        if (object) {
            int error = string(p, &key);
            if (error)
                return error;
            for (json_value *old = v->child; old; old = old->next)
                if (same(key, old->key)) {
                    free(key.data);
                    return 400;
                }
            space(p);
            if (p->at == p->length || p->data[p->at++] != ':') {
                free(key.data);
                return 400;
            }
            space(p);
        }
        json_value *item = value(p, depth);
        if (!item) {
            free(key.data);
            return p->error;
        }
        item->key = key;
        *tail = item;
        tail = &item->next;
        space(p);
        if (p->at == p->length)
            return 400;
        unsigned char c = p->data[p->at++];
        if (c == end)
            return 0;
        if (c != ',')
            return 400;
        space(p);
    }
}

static int digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}
static int number(parser *p, json_value *v)
{
    size_t start = p->at;
    if (p->data[p->at] == '-')
        p->at++;
    if (p->at == p->length)
        return 400;
    if (p->data[p->at] == '0')
        p->at++;
    else {
        if (!digit(p->data[p->at]))
            return 400;
        while (p->at < p->length && digit(p->data[p->at]))
            p->at++;
    }
    if (p->at < p->length && p->data[p->at] == '.') {
        p->at++;
        size_t first = p->at;
        while (p->at < p->length && digit(p->data[p->at]))
            p->at++;
        if (first == p->at)
            return 400;
    }
    if (p->at < p->length && (p->data[p->at] == 'e' || p->data[p->at] == 'E')) {
        p->at++;
        if (p->at < p->length && (p->data[p->at] == '+' || p->data[p->at] == '-'))
            p->at++;
        size_t first = p->at;
        while (p->at < p->length && digit(p->data[p->at]))
            p->at++;
        if (first == p->at)
            return 400;
    }
    v->type = JSON_NUMBER;
    v->text.length = p->at - start;
    v->text.data = malloc(v->text.length);
    if (!v->text.data)
        return 500;
    memcpy(v->text.data, p->data + start, v->text.length);
    return 0;
}

static json_value *value(parser *p, size_t depth)
{
    space(p);
    if (p->at == p->length) {
        p->error = 400;
        return NULL;
    }
    if (++p->values > JSON_VALUES) {
        p->error = 413;
        return NULL;
    }
    json_value *v = calloc(1, sizeof(*v));
    if (!v) {
        p->error = 500;
        return NULL;
    }
    unsigned char c = p->data[p->at];
    int error = 0;
    if (c == '{' || c == '[')
        error = container(p, v, depth);
    else if (c == '"') {
        v->type = JSON_STRING;
        error = string(p, &v->text);
    } else if (c == '-' || digit(c))
        error = number(p, v);
    else {
        const char *word = c == 'n' ? "null" : c == 't' ? "true" : c == 'f' ? "false" : "";
        size_t n = strlen(word);
        if (!n || n > p->length - p->at || memcmp(p->data + p->at, word, n))
            error = 400;
        else {
            p->at += n;
            v->type = c == 'n' ? JSON_NULL : JSON_BOOL;
            v->boolean = c == 't';
        }
    }
    if (error) {
        json_free(v);
        p->error = error;
        return NULL;
    }
    return v;
}

int json_parse(const unsigned char *data, size_t length, json_value **out)
{
    *out = NULL;
    parser p = {.data = data, .length = length};
    json_value *v = value(&p, 0);
    if (!v)
        return p.error;
    space(&p);
    if (p.at != length) {
        json_free(v);
        return 400;
    }
    *out = v;
    return 0;
}

const json_value *json_member(const json_value *object, const char *name)
{
    if (!object || object->type != JSON_OBJECT)
        return NULL;
    json_bytes key = {(unsigned char *)name, strlen(name)};
    for (const json_value *v = object->child; v; v = v->next)
        if (same(v->key, key))
            return v;
    return NULL;
}

int json_quote(json_output *out, const unsigned char *text, size_t length)
{
    int error = json_append(out, "\"", 1);
    static const char hexchars[] = "0123456789abcdef";
    for (size_t i = 0; !error && i < length; i++) {
        unsigned char c = text[i];
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            error = json_append(out, pair, 2);
        } else if (c < 32) {
            char escape[6] = {'\\', 'u', '0', '0', hexchars[c >> 4], hexchars[c & 15]};
            error = json_append(out, escape, 6);
        } else
            error = json_append(out, (const char *)&c, 1);
    }
    return error ? error : json_append(out, "\"", 1);
}

int json_write(json_output *out, const json_value *v)
{
    switch (v->type) {
    case JSON_NULL:
        return json_append(out, "null", 4);
    case JSON_BOOL:
        return json_append(out, v->boolean ? "true" : "false", v->boolean ? 4 : 5);
    case JSON_STRING:
        return json_quote(out, v->text.data, v->text.length);
    case JSON_NUMBER:
        return json_append(out, (const char *)v->text.data, v->text.length);
    default:
        break;
    }
    int object = v->type == JSON_OBJECT, error = json_append(out, object ? "{" : "[", 1), first = 1;
    for (const json_value *item = v->child; item && !error; item = item->next) {
        if (!first)
            error = json_append(out, ",", 1);
        first = 0;
        if (object && !error)
            error = json_quote(out, item->key.data, item->key.length);
        if (object && !error)
            error = json_append(out, ":", 1);
        if (!error)
            error = json_write(out, item);
    }
    return error ? error : json_append(out, object ? "}" : "]", 1);
}

static int form_decode(const unsigned char *data, size_t length, json_bytes *text)
{
    json_output out = {0};
    for (size_t i = 0; i < length; i++) {
        unsigned char c = data[i];
        if (c == '%') {
            if (i + 2 >= length || hex(data[i + 1]) < 0 || hex(data[i + 2]) < 0) {
                free(out.data);
                return 400;
            }
            c = (unsigned char)(hex(data[i + 1]) * 16 + hex(data[i + 2]));
            i += 2;
        } else if (c == '+')
            c = ' ';
        if (json_append(&out, (char *)&c, 1)) {
            free(out.data);
            return 500;
        }
    }
    for (size_t i = 0; i < out.length;) {
        size_t n = utf8_width((unsigned char *)out.data + i, out.length - i);
        if (!n) {
            free(out.data);
            return 400;
        }
        i += n;
    }
    if (!out.data && json_append(&out, "", 0))
        return 500;
    text->data = (unsigned char *)out.data;
    text->length = out.length;
    return 0;
}

int form_parse(const unsigned char *data, size_t length, json_value **out)
{
    *out = NULL;
    json_value *root = calloc(1, sizeof(*root));
    if (!root)
        return 500;
    root->type = JSON_OBJECT;
    json_value **tail = &root->child;
    size_t count = 0;
    int error = 0;
    for (size_t start = 0; start < length;) {
        if (++count > 100) {
            error = 413;
            break;
        }
        size_t end = start;
        while (end < length && data[end] != '&')
            end++;
        size_t equal = start;
        while (equal < end && data[equal] != '=')
            equal++;
        json_value *item = calloc(1, sizeof(*item));
        if (!item) {
            error = 500;
            break;
        }
        item->type = JSON_STRING;
        error = form_decode(data + start, equal - start, &item->key);
        if (!error)
            error = form_decode(data + (equal < end ? equal + 1 : end),
                                equal < end ? end - equal - 1 : 0, &item->text);
        if (!error)
            for (json_value *old = root->child; old; old = old->next)
                if (same(old->key, item->key)) {
                    error = 400;
                    break;
                }
        if (error) {
            json_free(item);
            break;
        }
        *tail = item;
        tail = &item->next;
        start = end + 1;
    }
    if (error) {
        json_free(root);
        return error;
    }
    *out = root;
    return 0;
}
