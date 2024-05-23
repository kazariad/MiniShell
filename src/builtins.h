#pragma once

#include "parser.h"

struct builtin_redir {
    int pseudofd;
    int realfd;
    struct builtin_redir *next;
};

typedef int (*builtin_fn)(struct command *, struct builtin_redir const *redir);

/** Look up corresponding builtin function for a given command
 *  Built-ins simulate real programs while running entirely with-
 *  in the shell itself. They can perform important tasks that
 *  are not possible with separate child processes.
 */
extern builtin_fn get_builtin(struct command *cmd);

