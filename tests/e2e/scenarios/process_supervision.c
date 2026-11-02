#include "cdin/backend/process.h"
#include "scenario.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    cdin_process_options options;
    cdin_process *process = NULL;
    const char *emit_args[4];
    const char *sleep_args[4];
    const char *fail_args[4];
    char output[256];
    size_t read_count = 0u;
    int exit_code = -1;

    SCENARIO_CHECK(argc == 2);
    memset(&options, 0, sizeof(options));
    options.capture_stdout = 1;
    options.capture_stderr = 1;
    options.inherit_environment = 1;

    /* Shell metacharacters and spaces must remain literal argv data. */
    emit_args[0] = argv[1];
    emit_args[1] = "emit";
    emit_args[2] = "folder with spaces; $(not-a-shell)";
    emit_args[3] = NULL;
    SCENARIO_CHECK(cdin_process_spawn(emit_args, &options, &process) == 0);
    SCENARIO_STATUS(cdin_process_wait(process, 5000u, &exit_code), 1);
    SCENARIO_CHECK(exit_code == 0);
    SCENARIO_CHECK(cdin_process_read_stdout(process, output, sizeof(output), &read_count) == 0);
    SCENARIO_CHECK(read_count == strlen(emit_args[2]));
    SCENARIO_CHECK(memcmp(output, emit_args[2], read_count) == 0);
    cdin_process_destroy(process);
    process = NULL;

    /* A timeout is not mistaken for process completion. */
    sleep_args[0] = argv[1];
    sleep_args[1] = "sleep";
    sleep_args[2] = "3000";
    sleep_args[3] = NULL;
    SCENARIO_CHECK(cdin_process_spawn(sleep_args, &options, &process) == 0);
    SCENARIO_STATUS(cdin_process_wait(process, 10u, &exit_code), 0);
    SCENARIO_CHECK(cdin_process_terminate(process) == 0);
    SCENARIO_STATUS(cdin_process_wait(process, 5000u, &exit_code), 1);
    cdin_process_destroy(process);
    process = NULL;

    fail_args[0] = argv[1];
    fail_args[1] = "exit";
    fail_args[2] = "23";
    fail_args[3] = NULL;
    SCENARIO_CHECK(cdin_process_spawn(fail_args, &options, &process) == 0);
    SCENARIO_STATUS(cdin_process_wait(process, 5000u, &exit_code), 1);
    SCENARIO_CHECK(exit_code == 23);
    cdin_process_destroy(process);

    (void)puts("PASS process_supervision: argv isolation, timeout, terminate and exit status");
    return 0;
}
