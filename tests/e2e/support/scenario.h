#ifndef CDIN_BACKEND_E2E_SCENARIO_H
#define CDIN_BACKEND_E2E_SCENARIO_H

#include <stdio.h>

#define SCENARIO_CHECK(expression) do { \
    if (!(expression)) { \
        (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

#define SCENARIO_STATUS(expression, expected) do { \
    int scenario_actual_ = (int)(expression); \
    int scenario_expected_ = (int)(expected); \
    if (scenario_actual_ != scenario_expected_) { \
        (void)fprintf(stderr, "FAIL %s:%d: %s returned %d, expected %d\n", \
                      __FILE__, __LINE__, #expression, scenario_actual_, scenario_expected_); \
        return 1; \
    } \
} while (0)

int scenario_temp_directory(char *buffer, size_t capacity);
int scenario_path_join(const char *left, const char *right, char *buffer, size_t capacity);
void scenario_sleep_ms(unsigned milliseconds);
int scenario_remove_file(const char *path);
int scenario_remove_directory(const char *path);

#endif
