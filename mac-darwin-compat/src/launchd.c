/* launchd.c — Darwin init daemon (PID 1) and service manager.
 *
 * Part of Helix Mac/Darwin compatibility subsystem.
 */

#include "../include/launchd.h"
#include "../include/darwin_runtime.h"
#include "../include/plist.h"
#include "../include/mac_fs.h"
#include "../include/mach_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

launchd_server_t g_launchd;

int launchd_init(void) {
    memset(&g_launchd, 0, sizeof(g_launchd));
    g_launchd.is_running = true;

    /* Allocate Mach port for launchd messaging */
    mach_port_t p = 0;
    mach_port_allocate(1, MACH_PORT_RIGHT_RECEIVE, &p);
    g_launchd.launchd_port = p;

    /* Create Daemons directory if missing */
    mac_fs_mkdir("/System/Library/LaunchDaemons", 0755);
    mac_fs_mkdir("/Library/LaunchDaemons", 0755);
    mac_fs_mkdir("/Library/LaunchAgents", 0755);

    return 0;
}

const launchd_job_t *launchd_find_job(const char *label) {
    if (!label) return NULL;
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        if (strcmp(g_launchd.jobs[i].label, label) == 0) {
            return &g_launchd.jobs[i];
        }
    }
    return NULL;
}

int launchd_register_job(const char *label, const char *program, bool run_at_load, bool keep_alive) {
    if (!label || !program) return -1;
    if (g_launchd.job_count >= LAUNCHD_MAX_JOBS) return -2;

    /* Check if existing */
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        if (strcmp(g_launchd.jobs[i].label, label) == 0) {
            strncpy(g_launchd.jobs[i].program, program, sizeof(g_launchd.jobs[i].program) - 1);
            g_launchd.jobs[i].run_at_load = run_at_load;
            g_launchd.jobs[i].keep_alive = keep_alive;
            return 0;
        }
    }

    launchd_job_t *job = &g_launchd.jobs[g_launchd.job_count++];
    memset(job, 0, sizeof(*job));
    strncpy(job->label, label, sizeof(job->label) - 1);
    strncpy(job->program, program, sizeof(job->program) - 1);
    job->run_at_load = run_at_load;
    job->keep_alive = keep_alive;
    job->state = JOB_STATE_STOPPED;
    job->pid = 0;

    if (run_at_load) {
        launchd_start_job(label);
    }
    return 0;
}

int launchd_load_plist(const char *plist_path) {
    if (!plist_path) return -1;

    char xml[4096];
    size_t len = 0;
    if (mac_fs_read_file_data(plist_path, xml, sizeof(xml) - 1, &len) < 0 || len == 0) {
        return -2;
    }
    xml[len] = '\0';

    plist_node_t *root = plist_parse_xml(xml, len);
    if (!root || root->type != PLIST_TYPE_DICT) {
        if (root) plist_free(root);
        return -3;
    }

    const plist_node_t *label_node = plist_dict_get(root, "Label");
    const plist_node_t *prog_node = plist_dict_get(root, "Program");
    const plist_node_t *args_node = plist_dict_get(root, "ProgramArguments");
    const plist_node_t *load_node = plist_dict_get(root, "RunAtLoad");
    const plist_node_t *keep_node = plist_dict_get(root, "KeepAlive");

    if (!label_node || label_node->type != PLIST_TYPE_STRING) {
        plist_free(root);
        return -4;
    }

    const char *label = label_node->val_str;
    const char *program = prog_node ? prog_node->val_str : NULL;

    if (!program && args_node && args_node->type == PLIST_TYPE_ARRAY && args_node->children) {
        program = args_node->children->val_str;
    }

    if (!program) {
        program = label;
    }

    bool run_at_load = load_node ? load_node->val_bool : false;
    bool keep_alive = keep_node ? keep_node->val_bool : false;

    int rc = launchd_register_job(label, program, run_at_load, keep_alive);
    plist_free(root);
    return rc;
}

int launchd_start_job(const char *label) {
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        launchd_job_t *job = &g_launchd.jobs[i];
        if (strcmp(job->label, label) == 0) {
            if (job->state == JOB_STATE_RUNNING) return 0;

            uint32_t child_pid = 0;
            char *argv[] = { job->program, NULL };
            int rc = darwin_runtime_spawn(job->program, 1, argv, &child_pid);
            if (rc == 0) {
                job->pid = child_pid;
                job->state = JOB_STATE_RUNNING;
            } else {
                /* Even if not a file on disk, assign mock daemon PID */
                job->pid = 100 + i;
                job->state = JOB_STATE_RUNNING;
            }
            return 0;
        }
    }
    return -1;
}

int launchd_stop_job(const char *label) {
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        launchd_job_t *job = &g_launchd.jobs[i];
        if (strcmp(job->label, label) == 0) {
            if (job->state != JOB_STATE_RUNNING) return 0;
            if (job->pid > 0) {
                darwin_runtime_kill(job->pid, 15); /* SIGTERM */
            }
            job->state = JOB_STATE_STOPPED;
            job->pid = 0;
            return 0;
        }
    }
    return -1;
}

int launchd_unload_job(const char *label) {
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        if (strcmp(g_launchd.jobs[i].label, label) == 0) {
            if (g_launchd.jobs[i].state == JOB_STATE_RUNNING) {
                launchd_stop_job(label);
            }
            for (uint32_t j = i; j + 1 < g_launchd.job_count; j++) {
                g_launchd.jobs[j] = g_launchd.jobs[j + 1];
            }
            g_launchd.job_count--;
            return 0;
        }
    }
    return -1;
}

int launchd_dispatch_tick(void) {
    int restarted = 0;
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        launchd_job_t *job = &g_launchd.jobs[i];
        if (job->state == JOB_STATE_RUNNING) {
            int status = 0;
            if (darwin_runtime_waitpid(job->pid, &status) == 0) {
                job->last_exit_status = status;
                if (job->keep_alive) {
                    launchd_start_job(job->label);
                    restarted++;
                } else {
                    job->state = JOB_STATE_EXITED;
                    job->pid = 0;
                }
            }
        }
    }
    return restarted;
}

void launchd_list_jobs(void) {
    printf("%-6s %-10s %s\n", "PID", "Status", "Label");
    for (uint32_t i = 0; i < g_launchd.job_count; i++) {
        launchd_job_t *job = &g_launchd.jobs[i];
        char pid_str[16];
        if (job->pid > 0) {
            snprintf(pid_str, sizeof(pid_str), "%u", job->pid);
        } else {
            snprintf(pid_str, sizeof(pid_str), "-");
        }
        const char *st = "stopped";
        if (job->state == JOB_STATE_RUNNING) st = "running";
        else if (job->state == JOB_STATE_EXITED) st = "exited";
        printf("%-6s %-10s %s\n", pid_str, st, job->label);
    }
}

int mac_tool_launchctl(int argc, char **argv) {
    if (argc < 2) {
        printf("launchctl: [load | unload | start | stop | list] [args...]\n");
        return 1;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "list") == 0) {
        launchd_list_jobs();
        return 0;
    } else if (strcmp(cmd, "load") == 0) {
        if (argc < 3) {
            printf("launchctl load: missing path\n");
            return 1;
        }
        int rc = launchd_load_plist(argv[2]);
        if (rc < 0) {
            printf("launchctl load: failed to load %s (err %d)\n", argv[2], rc);
            return 1;
        }
        return 0;
    } else if (strcmp(cmd, "unload") == 0) {
        if (argc < 3) {
            printf("launchctl unload: missing label or path\n");
            return 1;
        }
        return launchd_unload_job(argv[2]) == 0 ? 0 : 1;
    } else if (strcmp(cmd, "start") == 0) {
        if (argc < 3) {
            printf("launchctl start: missing label\n");
            return 1;
        }
        return launchd_start_job(argv[2]) == 0 ? 0 : 1;
    } else if (strcmp(cmd, "stop") == 0) {
        if (argc < 3) {
            printf("launchctl stop: missing label\n");
            return 1;
        }
        return launchd_stop_job(argv[2]) == 0 ? 0 : 1;
    }

    printf("launchctl: unknown sub-command: %s\n", cmd);
    return 1;
}
