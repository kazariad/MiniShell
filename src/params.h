#pragma once

#include <sys/types.h>

struct params {
    int status;
    pid_t bg_pid;
};

/* Declaration for a struct holding the two special parameters we're using in our
 * shell: status ($?) and last bg pid ($!).
 */
extern struct params params;
