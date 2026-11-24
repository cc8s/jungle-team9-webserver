#include "json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const char *text, int expected)
{
    json_value *v = NULL;
    int status = json_parse((const unsigned char *)text, strlen(text), &v);
    assert(status == expected);
    json_free(v);
}
int main(void)
{
    const char *text = "{\"name\":\"홍길동\",\"age\":9007199254740993,\"x\":[true,false,null,-1."
                       "25e+3],\"zero\":\"a\\u0000b\",\"emoji\":\"\\ud83d\\ude00\"}";
    json_value *v = NULL;
    assert(json_parse((const unsigned char *)text, strlen(text), &v) == 0);
    const json_value *zero = json_member(v, "zero");
    assert(zero && zero->text.length == 3 && zero->text.data[1] == 0);
    json_output out = {0};
    assert(!json_write(&out, v));
    assert(strstr(out.data, "9007199254740993") && strstr(out.data, "a\\u0000b"));
    json_value *copy = NULL;
    assert(!json_parse((unsigned char *)out.data, out.length, &copy));
    json_free(v);
    json_free(copy);
    free(out.data);
    expect("{\"a\":1,\"\\u0061\":2}", 400);
    expect("{\"x\":\"\\ud800\"}", 400);
    expect("\"\\udc00\"", 400);
    expect("\"\xc0\x80\"", 400);
    expect("\"\xed\xa0\x80\"", 400);
    expect("01", 400);
    expect("1.", 400);
    expect("1e+", 400);
    expect("[1,]", 400);
    expect("true trailing", 400);
    expect("", 400);
    char depth[40];
    memset(depth, '[', 17);
    memset(depth + 17, ']', 17);
    depth[34] = 0;
    expect(depth, 413);
    depth[16] = ']';
    memset(depth + 17, ']', 15);
    depth[32] = 0;
    expect(depth, 0);
    for (int n = 100; n <= 101; n++) {
        out = (json_output){0};
        json_append(&out, "{", 1);
        for (int i = 0; i < n; i++) {
            char item[40];
            int len = snprintf(item, sizeof(item), "%s\"k%d\":0", i ? "," : "", i);
            json_append(&out, item, (size_t)len);
        }
        json_append(&out, "}", 1);
        expect(out.data, n == 100 ? 0 : 413);
        free(out.data);
    }
    for (int n = 1000; n <= 1001; n++) {
        out = (json_output){0};
        json_append(&out, "[", 1);
        for (int i = 0; i < n; i++)
            json_append(&out, i ? ",0" : "0", i ? 2 : 1);
        json_append(&out, "]", 1);
        expect(out.data, n == 1000 ? 0 : 413);
        free(out.data);
    }
    for (int total = 10000; total <= 10001; total++) {
        out = (json_output){0};
        json_append(&out, "[", 1);
        for (int a = 0; a < 10; a++) {
            json_append(&out, a ? ",[" : "[", a ? 2 : 1);
            int count = a == 9 ? (total == 10000 ? 998 : 999) : 999;
            for (int i = 0; i < count; i++)
                json_append(&out, i ? ",0" : "0", i ? 2 : 1);
            json_append(&out, "]", 1);
        }
        json_append(&out, "]", 1);
        expect(out.data, total == 10000 ? 0 : 413);
        free(out.data);
    }
    const char *form = "name=%ED%99%8D%EA%B8%B8%EB%8F%99&note=a+b";
    assert(!form_parse((unsigned char *)form, strlen(form), &v));
    assert(json_member(v, "name")->text.length == 9);
    json_free(v);
    const char *nul_forms[] = {"key=%00", "%00=value", "a=ok&b=x%00y", "a%00b=value"};
    for (size_t i = 0; i < sizeof(nul_forms) / sizeof(nul_forms[0]); i++) {
        assert(form_parse((const unsigned char *)nul_forms[i], strlen(nul_forms[i]), &v) == 400);
        assert(v == NULL);
    }
    const unsigned char raw_value[] = {'k', '=', 'a', 0, 'b'};
    const unsigned char raw_key[] = {'k', 0, '=', 'v'};
    assert(form_parse(raw_value, sizeof(raw_value), &v) == 400 && v == NULL);
    assert(form_parse(raw_key, sizeof(raw_key), &v) == 400 && v == NULL);
    assert(form_parse((unsigned char *)"name=a&%6eame=b", 14, &v) == 400);
    assert(form_parse((unsigned char *)"x=%zz", 5, &v) == 400);
    for (int n = 100; n <= 101; n++) {
        out = (json_output){0};
        for (int i = 0; i < n; i++) {
            char item[30];
            int len = snprintf(item, sizeof(item), "%sk%d=v", i ? "&" : "", i);
            json_append(&out, item, (size_t)len);
        }
        assert(form_parse((unsigned char *)out.data, out.length, &v) == (n == 100 ? 0 : 413));
        json_free(v);
        free(out.data);
    }
    puts("JSON/form: grammar, UTF-8, duplicate keys and complexity limits passed");
}
