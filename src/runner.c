#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtins.h"
#include "expand.h"
#include "jobs.h"
#include "params.h"
#include "parser.h"
#include "signal.h"
#include "vars.h"
#include "wait.h"

#include "runner.h"

/* Expands all the command words in a command
 *
 * This is:
 *   cmd->words[i]
 *      ; i from 0 to cmd->word_count
 *
 *   cmd->assignments[i]->value
 *      ; i from 0 to cmd->assignment_count
 *
 *   cmd->io_redirs[i]->filename
 *      ; i from 0 to cmd->io_redir_count
 *
 * */
static int
expand_command_words(struct command *cmd) {
    for (size_t i = 0; i < cmd->word_count; ++i) {
        expand(&cmd->words[i]);
    }

    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        expand(&cmd->assignments[i]->value);
    }

    for (size_t i = 0; i < cmd->io_redir_count; ++i) {
        expand(&cmd->io_redirs[i]->filename);
    }
    return 0;
}

/** Performs variable assignments before running a command
 *
 * @param cmd        the command to be executed
 * @param export_all controls whether variables are also exported
 *
 * if export_all is zero, variables are assigned but not exported.
 * if export_all is non-zero, variables are assigned and exported.
 */
static int
do_variable_assignment(struct command const *cmd, int export_all) {
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        struct assignment *a = cmd->assignments[i];
        if (vars_set(a->name, a->value) != 0) return -1;
        if (export_all != 0) {
            if (vars_export(a->name) != 0) return -1;
        }
    }
    return 0;
}

static int
get_io_flags(enum io_operator io_op) {
    int flags = 0;
    /*
     *  Here is the specified behavior:
     *    * All operators with a '<'
     *       - open for reading
     *    * All operators with a '>'
     *       - open for writing
     *       - create if doesn't exist (mode 0777)
     *
     *    * operator '>'
     *       - fail if file exists
     *    * operator '>>'
     *       - open in append mode
     *    * operator '>|'
     *       - truncate file if it exists
     *
     * The operators <& and >& are treated the same as < and >, respectively.
     *
     * based on: Redirection. Shell Command Language. Shell & Utilities.
     * POSIX 1.2008
     */
    switch (io_op) {
        case OP_LESSAND: /* <& */
        case OP_LESS:    /* < */
            flags = O_RDONLY;
            break;
        case OP_GREATAND: /* >& */
        case OP_GREAT:    /* > */
            flags = O_WRONLY | O_CREAT | O_EXCL;
            break;
        case OP_DGREAT: /* >> */
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        case OP_LESSGREAT: /* <> */
            flags = O_RDWR | O_CREAT;
            break;
        case OP_CLOBBER: /* >| */
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
    }
    return flags;
}

/** moves a file descriptor
 *
 * @param src  the source file descriptor
 * @param dst  the target file descriptor
 * @returns    dst on success, -1 on failure
 *
 * src is moved to dst, and src is closed.
 *
 * If failure occurs, src and dst are unchanged.
 */
static int
move_fd(int src, int dst) {
    if (src == dst) return dst;
    if (dup2(src, dst) == -1) return -1;
    if (close(src) == -1) return -1;
    return dst;
}

/** Performs i/o pseudo-redirection for builtin commands
 *
 * @param [in]cmd the command we are performing redirections for.
 * @param [out]redir_list a virtual file descriptor table on top of the shell's
 * own file descriptors.
 *
 * This function performs all of the normal i/o redirection, but doesn't
 * overwrite any existing open files. Instead, it performs virtual redirections,
 * maintainig a list of what /would/ have changed if the redirection was
 * actually performed. The builtins refer to this list to access the correct
 * file descriptors for i/o.
 *
 * This allows the redirections to be undone after executing a builtin, which is
 * necessary to avoid screwing up the shell, since builtins don't run as
 * separate child processes--they are just functions that are a part of the
 * shell itself.
 */
static int
do_builtin_io_redirects(struct command *cmd, struct builtin_redir **redir_list) {
    int status = 0;
    for (size_t i = 0; i < cmd->io_redir_count; ++i) {
        struct io_redir *r = cmd->io_redirs[i];
        if (r->io_op == OP_GREATAND || r->io_op == OP_LESSAND) {
            if (strcmp(r->filename, "-") == 0) {
                /* [n]>&- and [n]<&- close file descriptor [n] */
                struct builtin_redir *rec = *redir_list;
                for (; rec; rec = rec->next) {
                    if (rec->pseudofd == r->io_number) {
                        close(rec->realfd);
                        rec->pseudofd = -1;
                        break;
                    }
                }
                if (rec == 0) {
                    rec = malloc(sizeof *rec);
                    if (!rec) goto err;
                    rec->pseudofd = r->io_number;
                    rec->realfd = -1;
                    rec->next = *redir_list;
                    *redir_list = rec;
                }
            } else {
                char *end = r->filename;
                long src = strtol(r->filename, &end, 10);

                if (*(r->filename) && !*end && src <= INT_MAX) {
                    for (struct builtin_redir *rec = *redir_list; rec; rec = rec->next) {
                        if (rec->realfd == src) {
                            errno = EBADF;
                            goto err;
                        }
                        if (rec->pseudofd == src) src = rec->realfd;
                    }
                    struct builtin_redir *rec = *redir_list;
                    for (; rec; rec = rec->next) {
                        if (rec->pseudofd == r->io_number) {
                            if (dup2(src, rec->realfd) < 0) goto err;
                            break;
                        }
                    }
                    if (rec == 0) {
                        rec = malloc(sizeof *rec);
                        if (!rec) goto err;
                        rec->pseudofd = r->io_number;
                        rec->realfd = dup(src);
                        rec->next = *redir_list;
                        *redir_list = rec;
                    }
                } else {
                    goto file_open;
                }
            }
        } else {
            file_open:;
            int flags = get_io_flags(r->io_op);
            int fd = open(r->filename, flags, 0777);
            if (fd < 0) goto err;
            struct builtin_redir *rec = *redir_list;
            for (; rec; rec = rec->next) {
                if (rec->pseudofd == r->io_number) {
                    if (move_fd(fd, rec->realfd) < 0) goto err;
                    break;
                }
            }
            if (rec == 0) {
                rec = malloc(sizeof *rec);
                if (!rec) goto err;
                rec->pseudofd = r->io_number;
                rec->realfd = fd;
                rec->next = *redir_list;
                *redir_list = rec;
            }
        }
        if (0) {
            err:
            status = -1;
        }
    }
    return status;
}

/** perform the main task of io redirection (for non-builtin commands)
 *
 * @param [in]cmd the command we are performing redirections for.
 * @returns 0 on success, -1 on failure
 *
 * Unlike the builtin redirections, this is straightforward, because it
 * will only ever happen in forked child processes--and can't affect the shell
 * itself.
 */
static int
do_io_redirects(struct command *cmd) {
    int status = 0;
    for (size_t i = 0; i < cmd->io_redir_count; ++i) {
        struct io_redir *r = cmd->io_redirs[i];
        if (r->io_op == OP_GREATAND || r->io_op == OP_LESSAND) {
            if (strcmp(r->filename, "-") == 0) {
                /* [n]>&- and [n]<&- close file descriptor [n] */
                if (close(r->io_number) == -1) goto err;
            } else {
                char *end = r->filename;
                long src = strtol(r->filename, &end, 10);

                if (*(r->filename) && !*end && src <= INT_MAX) {
                    if (dup2((int) src, r->io_number) == -1) goto err;
                } else {
                    goto file_open; /* XXX target is just a few lines below this */
                }
            }
        } else {
            file_open:;
            int flags = get_io_flags(r->io_op);
            int fd = open(r->filename, flags, 0777);
            if (fd == -1) goto err;
            if (move_fd(fd, r->io_number) == -1) goto err;
        }
        if (0) {
            err: /* Anything that can fail should jump here. No silent errors!!! */
            status = -1;
        }
    }
    return status;
}

int
run_command_list(struct command_list *cl) {
    int pipeline_fds[2] = {-1, -1};
    pid_t pipeline_pgid = 0;
    jid_t pipeline_jid = -1;

    for (size_t i = 0; i < cl->command_count; ++i) {
        struct command *cmd = cl->commands[i];
        expand_command_words(cmd);

        // 3 control types:
        // ';' -- foreground command, parent waits sychronously for child process
        // '&' -- background command, parent waits asynchronously for child process
        // '|' -- pipeline command, behaves as a background command, and writes stdout to a pipe
        //
        // From the perspective of child processes, foreground/background is the same; it is
        // solely a question of whether the parent waits or not
        //
        // Two command types:
        // External -- these are actual standalone programs that are executed with exec()
        // Builtins -- these are routines that are implemented as part of the shell, itself.
        //
        // Importantly, builtin commands do not fork() when they are run as
        // foreground commands. This is because they must run in the shell's own
        // execution environment (not as children) in order to modify it. For
        // example to change the shell's working directory, exit the shell, and so
        // on.

        int const is_pl = cmd->ctrl_op == '|'; /* pipeline */
        int const is_bg = cmd->ctrl_op == '&'; /* background */
        int const is_fg = cmd->ctrl_op == ';'; /* foreground */
        assert(is_pl || is_bg || is_fg);

        int stdin_override = pipeline_fds[STDIN_FILENO];

        if (is_pl) {
            if (pipe(pipeline_fds) == -1) err(1, 0);
        } else {
            pipeline_fds[0] = -1;
            pipeline_fds[1] = -1;
        }

        int stdout_override = pipeline_fds[STDOUT_FILENO];

        builtin_fn builtin = get_builtin(cmd);

        pid_t child_pid = 0;

        if (builtin == NULL || !is_fg) {
            if ((child_pid = fork()) == -1) err(1, 0);
        }

        if (child_pid == 0) {
            if (builtin) {
                struct builtin_redir *redir_list = 0;

                if (stdin_override >= 0) {
                    struct builtin_redir *rec = malloc(sizeof *rec);
                    if (!rec) goto err;
                    rec->pseudofd = STDIN_FILENO;
                    rec->realfd = stdin_override;
                    rec->next = redir_list;
                    redir_list = rec;
                }
                if (stdout_override >= 0) {
                    struct builtin_redir *rec = malloc(sizeof *rec);
                    if (!rec) goto err;
                    rec->pseudofd = STDOUT_FILENO;
                    rec->realfd = stdout_override;
                    rec->next = redir_list;
                    redir_list = rec;
                }

                do_builtin_io_redirects(cmd, &redir_list);

                do_variable_assignment(cmd, 0);

                int result = builtin(cmd, redir_list);

                while (redir_list) {
                    close(redir_list->realfd);
                    void *tmp = redir_list;
                    redir_list = redir_list->next;
                    free(tmp);
                }

                params.status = result ? 127 : 0;
                if (!is_fg) exit(params.status);

                errno = 0;
                continue;
            } else {
                if (stdin_override >= 0) {
                    if (move_fd(stdin_override, STDIN_FILENO) == -1) err(1, 0);
                }

                if (stdout_override >= 0) {
                    if (move_fd(stdout_override, STDOUT_FILENO) == -1) err(1, 0);
                }

                if (do_io_redirects(cmd) < 0) err(1, 0);

                if (do_variable_assignment(cmd, 1) < 0) err(1, 0);

                if (signal_restore() < 0) err(1, 0);

                execvp(cmd->words[0], cmd->words);

                err(127, 0);
                assert(0);
            }
        }
        assert(child_pid > 0);
        if (stdout_override >= 0) close(stdout_override);
        if (stdin_override >= 0) close(stdin_override);

        if (setpgid(child_pid, pipeline_pgid) < 0) goto err;
        if (pipeline_pgid == 0) {
            /* Start of a new pipeline */
            assert(child_pid == getpgid(child_pid));
            pipeline_pgid = child_pid;
            pipeline_jid = jobs_add(pipeline_pgid);
            if (pipeline_jid < 0) goto err;
        }

        if (is_fg) {
            if (wait_on_fg_gid(pipeline_pgid) < 0) {
                warn(0);
                params.status = 127;
                return -1;
            }
        } else {
            params.bg_pid = child_pid;

            if (is_bg) {
                /* Pipelines that end with a background (&) command print a little
                 * message when they spawn.
                 * "[<JOBID>] <GROUPID>\n"
                 */
                fprintf(stderr,
                        "[%jd] %jd\n",
                        (intmax_t) pipeline_jid,
                        (intmax_t) pipeline_pgid);
            }
            params.status = 0;
        }

        if (!is_pl) {
            pipeline_pgid = 0;
        }
    }

    return 0;
    err:
    return -1;
}
