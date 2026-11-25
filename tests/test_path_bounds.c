/* Compile the real private decoder here without exporting a production test API. */
#define http_send_all bounds_send_all
#define http_respond bounds_respond
#define http_send_response bounds_send_response
#define http_prepare_response included_test_path_bounds_http_prepare_response
#define http_prepare_unavailable included_test_path_bounds_http_prepare_unavailable
#define http_prepare_file_chunk included_test_path_bounds_http_prepare_file_chunk
#define http_prepared_dispose included_test_path_bounds_http_prepared_dispose
#include "../src/http_response.c"
#include <assert.h>

int main(void)
{
    const struct { const char *target; size_t size; int status; const char *path; } cases[] = {
        {"/a", 0, 414, NULL}, {"/a", 1, 414, NULL}, {"/a", 2, 414, NULL},
        {"/a", 3, 0, "/a"}, {"/%61?ignored", 3, 0, "/a"},
        {"/", 2, 414, NULL}, {"/", sizeof("/index.html")-1, 414, NULL},
        {"/", sizeof("/index.html"), 0, "/index.html"},
        {"/%", 32, 400, NULL}, {"/%0", 32, 400, NULL},
        {"/%00", 32, 400, NULL}, {"/../a", 32, 403, NULL}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        unsigned char output[40]; memset(output, 0xa5, sizeof(output));
        assert(decode_path((const unsigned char *)cases[i].target, strlen(cases[i].target),
                           (char *)output + 1, cases[i].size) == cases[i].status);
        assert(output[0] == 0xa5);
        for (size_t j = cases[i].size + 1; j < sizeof(output); j++) assert(output[j] == 0xa5);
        if (!cases[i].status) assert(!strcmp((char *)output+1, cases[i].path));
    }
    puts("Path decoder: capacity, terminator, index substitution and canaries passed");
}
