#pragma once

#include <sys/types.h>

/* Job id type */
typedef long jid_t;

struct job {
    jid_t jid;  /* Job id */
    pid_t pgid; /* Process group id */
};

/** Gets a list of all jobs
 *
 * Invalidated by a call to jobs_add or jobs_remove
 */
extern struct job const *jobs_get_joblist(void);

/** Gets the size of the job list
 *
 * Invalidated by a call to jobs_add or jobs_remove
 */
extern size_t jobs_get_joblist_size(void);

/** Add a process group to the jobs list
 *
 * @param [in]pgid the process group id to add to the job list
 * @returns the new job id, or -1 on failure
 */
extern jid_t jobs_add(pid_t pgid);

/** Removes a process group from the jobs list
 *
 * @param [in]pgid the process group id to remove from the job list
 * @returns 0 on success, -1 on failure
 */
extern int jobs_remove_gid(pid_t pgid);

/** Looks up a job's job id
 *
 * @param [in]pgid the process group id to look up
 * @returns The job id on success, -1 on failure
 */
extern jid_t jobs_get_jid(pid_t pgid);

/** Looks up a job's process group id
 *
 * @param [in]jobid the job id to look up
 * @returns The process group id on success, -1 on failure
 */
extern pid_t jobs_get_gid(jid_t jobid);

/** Cleans up any resources associated with jobs tracking */
extern void jobs_cleanup(void);
