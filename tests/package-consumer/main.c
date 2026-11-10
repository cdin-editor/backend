#include <cdin/backend.h>

#include <string.h>

int main(void)
{
    cdin_backend_config config;
    const char *version;

    cdin_backend_config_init(&config);
    version = cdin_backend_version();

    if (config.struct_size < sizeof(cdin_backend_config)) return 1;
    if (version == NULL) return 2;
    if (strcmp(version, CDIN_BACKEND_VERSION_STRING) != 0) return 3;
    if (strcmp(version, "1.2.1") != 0) return 4;
    return 0;
}
