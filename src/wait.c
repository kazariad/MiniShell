#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jobs.h"
#include "params.h"
#include "wait.h"

int
wait_on_fg_gid(pid_t pgid) {
    if (pgid < 0) return -1;
    /* Make sure the foreground group is running */
    if (kill(pgid, SIGCONT) == -1) return -1;

    if (isatty(STDIN_FILENO)) {
        if (tcsetpgrp(STDIN_FILENO, pgid) == -1) return -1;
    } else {
        switch (errno) {
            case ENOTTY:
                errno = 0;
                break;
            default:
                return -1;
        }
    }

    int retval = 0;
    int last_status = 0;
    for (;;) {
        /* Wait on all processes in the process group 'pgid' */
        int status;
        pid_t res = waitpid(-pgid, &status, WSTOPPED);
        if (res < 0) {
            if (errno == ECHILD) {
                errno = 0;
                if (WIFEXITED(last_status)) {
                    params.status = WEXITSTATUS(last_status);
                } else if (WIFSIGNALED(last_status)) {
                    params.status = 128 + WTERMSIG(last_status);
                }

                jobs_remove_gid(pgid);
                goto out;
            }
            goto err;
        }
        assert(res > 0);
        last_status = status;
        if (WIFSTOPPED(status)) {
            fprintf(stderr, "[%jd] Stopped\n", (intmax_t) jobs_get_jid(pgid));
            goto out;
        }
    }

    out:
    if (0) {
        err:
        retval = -1;
    }

    if (isatty(STDIN_FILENO)) {
        pid_t bspgid = getpgid(0);
        if (bspgid == -1) return -1;
        if (tcsetpgrp(STDIN_FILENO, bspgid) == -1) return -1;
    } else {
        switch (errno) {
            case ENOTTY:
                errno = 0;
                break;
            default:
                return -1;
        }
    }
    return retval;
}

int
wait_on_fg_job(jid_t jid) {
    pid_t pgid = jobs_get_gid(jid);
    if (pgid < 0) return -1;
    return wait_on_fg_gid(pgid);
}

int
wait_on_bg_jobs() {
    size_t job_count = jobs_get_joblist_size();
    struct job const *jobs = jobs_get_joblist();
    for (size_t i = 0; i < job_count; ++i) {
        pid_t pgid = jobs[i].pgid;
        jid_t jid = jobs[i].jid;
        int last_status = 0;
        for (;;) {
            int status;
            pid_t pid = waitpid(-pgid, &status, WSTOPPED | WNOHANG);
            if (pid == 0) {
                break;
            } else if (pid < 0) {
                if (errno == ECHILD) {
                    errno = 0;
                    if (WIFEXITED(last_status)) {
                        fprintf(stderr, "[%jd] Done\n", (intmax_t) jid);
                    } else if (WIFSIGNALED(last_status)) {
                        fprintf(stderr, "[%jd] Terminated\n", (intmax_t) jid);
                    }
                    jobs_remove_gid(pgid);
                    job_count = jobs_get_joblist_size();
                    jobs = jobs_get_joblist();
                    break;
                }
                return -1;
            }

            last_status = status;
            if (WIFSTOPPED(status)) {
                fprintf(stderr, "[%jd] Stopped\n", (intmax_t) jid);
                break;
            }
        }
    }
    return 0;
}
