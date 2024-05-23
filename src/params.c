#include "params.h"

/* Definition for a struct holding the two special parameters we're using in our
 * shell: status ($?) and last bg pid ($!).
 */
struct params params = {.status = 0, .bg_pid = 0};
