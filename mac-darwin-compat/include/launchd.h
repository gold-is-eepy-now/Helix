/* launchd.h — Darwin launchd daemon and service management (PID 1 init).
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#ifndef MAC_DARWIN_LAUNCHD_H
#define MAC_DARWIN_LAUNCHD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAUNCHD_PID              1
#define LAUNCHD_MAX_JOBS         32
#define LAUNCHD_MAX_LABEL_LEN    64
#define LAUNCHD_MAX_PATH_LEN     256
#define LAUNCHD_MAX_ARGS         8

typedef enum {
    JOB_STATE_STOPPED = 0,
    JOB_STATE_RUNNING,
    JOB_STATE_WAITING,
    JOB_STATE_EXITED,
    JOB_STATE_FAILED
} job_state_t;

typedef struct {
    char        label[LAUNCHD_MAX_LABEL_LEN];
    char        program[LAUNCHD_MAX_PATH_LEN];
    char       *args[LAUNCHD_MAX_ARGS];
    uint32_t    argc;
    bool        run_at_load;
    bool        keep_alive;
    bool        on_demand;
    job_state_t state;
    uint32_t    pid;
    int         last_exit_status;
    uint64_t    mach_port;
} launchd_job_t;

typedef struct {
    launchd_job_t jobs[LAUNCHD_MAX_JOBS];
    uint32_t      job_count;
    bool          is_running;
    uint64_t      launchd_port;
} launchd_server_t;

extern launchd_server_t g_launchd;

/* Core launchd APIs */
int  launchd_init(void);
int  launchd_load_plist(const char *plist_path);
int  launchd_register_job(const char *label, const char *program, bool run_at_load, bool keep_alive);
int  launchd_start_job(const char *label);
int  launchd_stop_job(const char *label);
int  launchd_unload_job(const char *label);
const launchd_job_t *launchd_find_job(const char *label);
int  launchd_dispatch_tick(void);
void launchd_list_jobs(void);

/* launchctl CLI implementation */
int  mac_tool_launchctl(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* MAC_DARWIN_LAUNCHD_H */
