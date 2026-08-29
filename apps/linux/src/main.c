#include <adwaita.h>
#include <curl/curl.h>
#include <string.h>

int vv_self_test(void);
int vv_app_run(int argc, char **argv);

int main(int argc, char **argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) return vv_self_test();
    if (argc > 1 && strcmp(argv[1], "--version") == 0) { g_print("VoiceVector %s\n", VV_VERSION); return 0; }
    return vv_app_run(argc, argv);
}
