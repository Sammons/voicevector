/* Headless entry: the core CLI (self-test, probes) — links without GTK so it
 * can run in any Linux CI container. */
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

int vv_self_test(void);

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) return vv_self_test();
    if (argc > 1 && strcmp(argv[1], "--version") == 0) { printf("VoiceVector core %s\n", VV_VERSION); return 0; }
    fprintf(stderr, "usage: voicevector-core --self-test | --version\n");
    return 2;
}
