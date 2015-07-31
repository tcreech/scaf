//
//  SCAFd server.
//  Tim Creech <tcreech@umd.edu>, University of Maryland, 2015
//
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>
#include <zmq.h>
#include <time.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "scaf.h"
#include "uthash.h"
#include "intpart.h"

#ifdef HAVE_LIBHWLOC
#include <hwloc.h>
static hwloc_topology_t topology;
static int num_hwloc_objs;
static hwloc_obj_t top_obj;
static hwloc_obj_type_t part_at = HWLOC_OBJ_PU;
#endif

typedef struct {
    int pid;
    int threads;
    void* current_section;
    unsigned checkins;
    double last_checkin_time;
    double last_expt_time;
    float metric;
    float log_factor;
    char name[SCAF_MAX_CLIENT_NAME_LEN+1];
    int malleable;
    int experimenting;
    int experiment_pid;
#ifdef HAVE_LIBHWLOC
    hwloc_cpuset_t affinity;
    hwloc_cpuset_t experiment_affinity;
#endif // HAVE_LIBHWLOC
    UT_hash_handle hh;
} scaf_client;

static int num_online_processors(void)
{
    char *maxenv = getenv("OMP_NUM_THREADS");
    if(maxenv)
        return atoi(maxenv);
    else
        return scaf_get_num_cpus();
}

static int chunksize = -1;

#define MAX_CLIENTS 8

#define REFEREE_PERIOD_US (250000)

#define DEFAULT_UNRESPONSIVE_THRESHOLD (10.0)

#define SERIAL_LOG_FACTOR (1.0)

#define RD_LOCK_CLIENTS pthread_rwlock_rdlock(&clients_lock)
#define RW_LOCK_CLIENTS pthread_rwlock_wrlock(&clients_lock)
#define UNLOCK_CLIENTS pthread_rwlock_unlock(&clients_lock)

static int nobgload = 0;
static int equipartitioning = 0;
static int experiment_equipartitioning = 0;
static int eq_offset = 0;
static int text_interface = 0;
static int unresponsive_threshold = DEFAULT_UNRESPONSIVE_THRESHOLD;
#if HAVE_LIBHWLOC
static int affinity = 1;
#else
static int affinity = 0;
#endif //HAVE_LIBHWLOC

static scaf_client *clients = NULL;

static int max_threads = -1;
static float bg_utilization;

static int stop_referee = 0;
static pthread_t referee, reaper, scoreboard, lookout;
static pthread_rwlock_t clients_lock;

static double startuptime;

static int inline get_nlwp(pid_t pid)
{
    int nlwp;
    FILE *fp;
    char statfile[128];
    sprintf(statfile, "/proc/%d/stat\0", pid);
    fp = fopen(statfile,"r");
    if(!fp)
        return 0;

    assert(1==fscanf(fp,"%*64d %*64s %*c %*64d %*64d %*64d %*64d %*64d %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64d %*64d %*64d %*64d %64d 0 %*64u %*64u %*64d %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64u %*64d %*64d %*64u %*64u %*64u %*64u %*64d\n",&nlwp));
    fclose(fp);
    return nlwp-2;
}

static void inline apply_affinity_partitioning(void)
{
    if(!affinity)
        return;
#ifdef HAVE_LIBHWLOC
    RD_LOCK_CLIENTS;

    unsigned non_malleable_count = 0;
    unsigned total_count = 0;
    scaf_client *current, *tmp;
    HASH_ITER(hh, clients, current, tmp) {
        non_malleable_count += (current->malleable ? 0 : 1);
        total_count++;
    }

    const int affinity_for_nonmalleable_only = 1;

    hwloc_cpuset_t client_total_set = hwloc_bitmap_alloc();
    hwloc_cpuset_t client_work_set = hwloc_bitmap_alloc();
    hwloc_cpuset_t client_experiment_set = hwloc_bitmap_alloc();

    // If there are no non-malleable clients, then just ensure that no clients
    // have any affinity and we are done.
    if(non_malleable_count < 1) {
        hwloc_bitmap_copy(client_total_set, hwloc_get_root_obj(topology)->cpuset);

        HASH_ITER(hh, clients, current, tmp) {
            // Only actually call the OS to change affinity if there is a change.
            if(!hwloc_bitmap_isequal(current->experiment_affinity, client_total_set)) {
                if(current->experimenting)
                    if(!affinity_for_nonmalleable_only || !current->malleable)
                        hwloc_set_proc_cpubind(topology, current->experiment_pid, client_total_set, HWLOC_CPUBIND_STRICT);
                hwloc_bitmap_copy(current->experiment_affinity, client_total_set);
            }
            // Only actually call the OS to change affinity if there is a change.
            if(!hwloc_bitmap_isequal(current->affinity, client_total_set)) {
                if(!affinity_for_nonmalleable_only || !current->malleable)
                    hwloc_set_proc_cpubind(topology, current->pid, client_total_set, HWLOC_CPUBIND_STRICT);
                hwloc_bitmap_copy(current->affinity, client_total_set);
                hwloc_bitmap_copy(current->affinity, client_total_set);
            }
        }

        // Finished! Everything is set to be scheduled anywhere on the machine.
        goto aap_finished;
    }

    hwloc_obj_t o = NULL;
    HASH_ITER(hh, clients, current, tmp) {
        hwloc_bitmap_zero(client_total_set);
        hwloc_bitmap_zero(client_work_set);
        hwloc_bitmap_zero(client_experiment_set);

        int weight;
        int current_threads;
        current_threads = current->threads;
        for(weight=0; weight < current_threads; weight++) {
            o = hwloc_get_next_obj_by_type(topology, part_at, o);
            hwloc_bitmap_or(client_total_set, client_total_set, o->cpuset);
        }
        assert(weight == hwloc_bitmap_weight(client_total_set));

        if(current->experimenting) {
            // If we are experimenting, do the allocation leaving one cpu for the
            // experiment.
            int first = hwloc_bitmap_first(client_total_set);
            hwloc_bitmap_copy(client_work_set, client_total_set);
            if(hwloc_bitmap_weight(client_work_set)>1)
                hwloc_bitmap_clr(client_work_set, first);
            hwloc_bitmap_set(client_experiment_set, first);

            // Only actually call the OS to change affinity if there is a change.
            if(!hwloc_bitmap_isequal(client_work_set, current->affinity)) {
                if(!affinity_for_nonmalleable_only || !current->malleable)
                    hwloc_set_proc_cpubind(topology, current->pid, client_work_set, HWLOC_CPUBIND_STRICT);
                hwloc_bitmap_copy(current->affinity, client_work_set);
            }
            if(!hwloc_bitmap_isequal(client_experiment_set, current->experiment_affinity)) {
                if(!affinity_for_nonmalleable_only || !current->malleable)
                    hwloc_set_proc_cpubind(topology, current->experiment_pid, client_experiment_set, HWLOC_CPUBIND_STRICT);
                hwloc_bitmap_copy(current->experiment_affinity, client_experiment_set);
            }
        } else {
            hwloc_bitmap_copy(client_work_set, client_total_set);
            // If not experimenting, do the same thing but give the main process
            // the whole set. Record the experiment's affinity to the same thing
            // becuase if a new one starts it will inherit it.
            if(!hwloc_bitmap_isequal(client_work_set, current->affinity)) {
                if(!affinity_for_nonmalleable_only || !current->malleable)
                    hwloc_set_proc_cpubind(topology, current->pid, client_work_set, HWLOC_CPUBIND_STRICT);
                hwloc_bitmap_copy(current->affinity, client_work_set);
                hwloc_bitmap_copy(current->experiment_affinity, client_work_set);
            }
        }
    }

aap_finished:
    hwloc_bitmap_free(client_total_set);
    hwloc_bitmap_free(client_work_set);
    hwloc_bitmap_free(client_experiment_set);
    UNLOCK_CLIENTS;
#endif
}

int get_scaf_controlled_pids(int** pid_list)
{
    RD_LOCK_CLIENTS;
    int size = HASH_COUNT(clients);
    *pid_list = malloc(sizeof(int)*size);

    scaf_client *current, *tmp;
    int i=0;
    HASH_ITER(hh, clients, current, tmp) {
        (*pid_list)[i] = current->pid;
        i++;
    }
    UNLOCK_CLIENTS;
    return size;
}

int pid_is_scaf_controlled(int pid, int* pid_list, int list_size)
{
    int i;
    int pgid = getpgid(pid);
    for(i=0; i<list_size; i++) {
        if(pid_list[i] == pid || pid_list[i] == pgid) {
            return 1;
        }
    }
    return 0;
}

// Go through a /proc/ filesystem and get jiffy usage for all processes not
// under scafd's control. This takes 1 second to run due to a 1 second sleep.
// Might possibly work on FreeBSD as is if we use /compat/linux/proc/ instead
// of /proc/. Can certainly be implemented for SunOS, but with mostly different
// code.
float proc_get_cpus_used(void)
{
    // Return immediately if the user disabled load monitoring
    if(nobgload) {
        sleep(1);
        return 0.0;
    }

    unsigned long g_user[2], g_low[2], g_sys[2], g_idle[2], g_iow[2], g_hirq[2], g_sirq[2];
    const char *format = "%d %s %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %lu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %lu %lu %llu";

    int* pid_list;
    int list_size = get_scaf_controlled_pids(&pid_list);
    unsigned long used_stime[2], used_utime[2], scaf_stime[2], scaf_utime[2];

    int z;
    for(z=0; z<2; z++) {
        FILE *fp;
        fp = fopen("/proc/stat","r");
        assert(fp && "Can't open $PROC/stat ?");
        assert(7==fscanf(fp,"cpu %32lu %32lu %32lu %32lu %32lu %32lu %32lu", &(g_user[z]), &(g_low[z]), &(g_sys[z]), &(g_idle[z]), &(g_iow[z]), &(g_hirq[z]), &(g_sirq[z])));
        fclose(fp);

        used_utime[z] = 0;
        used_stime[z] = 0;
        scaf_utime[z] = 0;
        scaf_stime[z] = 0;

        DIR* procroot;
        procroot = opendir("/proc");
        int name_max = pathconf("/proc", _PC_NAME_MAX);
        if(name_max == -1)
            name_max = 255;
        size_t len = offsetof(struct dirent, d_name) + name_max + 1;
        struct dirent *procdir = malloc(len);
        struct dirent *p_procdir = procdir;
        while(readdir_r(procroot, p_procdir, &procdir) == 0 && procdir != NULL) {
            p_procdir = procdir;
            int procdir_pid = atoi(procdir->d_name);
            if(procdir_pid == 0)
                continue;

            int is_scaf_pid = 0;
            if(pid_is_scaf_controlled(procdir_pid, pid_list, list_size)) {
                is_scaf_pid = 1;
            }

            FILE *stat_f;
            char buf[256];
            sprintf(buf, "/proc/%d/stat",procdir_pid);
            stat_f = fopen(buf, "r");
            struct proc_stat stat;
            if(stat_f) {
                struct proc_stat *s = &stat;
                // Parse the /proc/$PID/stat file for jiffie usage
                int ret = fscanf(stat_f, format, &s->pid, s->comm, &s->state, &s->ppid, &s->pgrp, &s->session, &s->tty_nr, &s->tpgid, &s->flags, &s->minflt, &s->cminflt, &s->majflt, &s->cmajflt, &s->utime, &s->stime, &s->cutime, &s->cstime, &s->priority, &s->nice, &s->num_threads, &s->itrealvalue, &s->starttime, &s->vsize, &s->rss, &s->rlim, &s->startcode, &s->endcode, &s->startstack, &s->kstkesp, &s->kstkeip, &s->signal, &s->blocked, &s->sigignore, &s->sigcatch, &s->wchan, &s->nswap, &s->cnswap, &s->exit_signal, &s->processor, &s->rt_priority, &s->policy, &s->delayacct_blkio_ticks);
                fclose(stat_f);
                if(ret == 42) {
                    if(is_scaf_pid) {
                        // If this process is scaf controlled, record its usage so
                        // that we can account for missing non-idle jiffies
                        scaf_utime[z] += stat.utime;
                        //scaf_stime[z] += stat.stime;
                    } else {
                        // If this is an uncontrolled process, record its usage so
                        // that we can avoid oversubscription later
                        used_utime[z] += stat.utime;
                        //used_stime[z] += stat.stime;
                    }
                } else {
                    // TODO: This happens a lot on the MIC. Why?
                }
            }
        }
        closedir(procroot);
        free(p_procdir);
        if(z!=1) {
            sleep(1);
            free(pid_list);
            list_size = get_scaf_controlled_pids(&pid_list);
        }
    }

    int long total_used = (g_user[1] - g_user[0]) + (g_low[1] - g_low[0]) + (g_sys[1] - g_sys[0]) + (g_iow[1] - g_iow[0]) + (g_hirq[1] - g_hirq[0]) + (g_sirq[1] - g_sirq[0]);
    int long scaf_used = (scaf_utime[1] - scaf_utime[0]) + (scaf_stime[1] - scaf_stime[0]);
    // Note that the scaf jiffies are reclaimed as "idle" here.
    int long total_idle = (g_idle[1] - g_idle[0]) + (scaf_used>0?scaf_used:0);

    int long non_scaf_used  = (used_utime[1] - used_utime[0]) + (used_stime[1] - used_stime[0]);
    (void)non_scaf_used; // Quiet the compiler
    int long used = total_used - scaf_used;
    int long total = used + total_idle;

    double utilization = (((double)used) / ((double)total));
    utilization = fmin(fmax(utilization,0.0), 1.0);
    utilization *= (double)max_threads;
    //printf("total_used: %ld; total_idle: %ld; used: %ld; total: %ld; util: %f\n", total_used, total_idle, used, total, utilization);

    free(pid_list);
    return utilization;
}

scaf_client *find_client(int client_pid)
{
    scaf_client* c;
    HASH_FIND_INT(clients, &client_pid, c);
    return c;
}

void get_name_from_pid(int pid, char *buf, size_t max)
{
    char commpath[64];
    sprintf(commpath, "/proc/%d/comm", pid);
    FILE *comm_f = fopen(commpath, "r");
    int s = 0;
    if(comm_f != NULL){
        char format[10];
        sprintf(format, "%%%ds\n", SCAF_MAX_CLIENT_NAME_LEN);
        s = fscanf(comm_f, format, buf);
    }
    if(s < 1) {
        char name[5] = "[??]\0";
        strncpy(buf, name, 5);
    }
    fclose(comm_f);
}

void add_client(int client_pid, int threads, void* client_section)
{
    scaf_client *c = (scaf_client*)malloc(sizeof(scaf_client));
    bzero((void*)c, sizeof(scaf_client));
    c->pid = client_pid;
    c->threads = threads;
    c->current_section = client_section;
    c->metric = 1.0;
    c->checkins = 1;
    c->last_checkin_time = rtclock();
    c->last_expt_time = rtclock();
    c->malleable = 1;
    c->experimenting = 0;
#ifdef HAVE_LIBHWLOC
    c->affinity = hwloc_bitmap_alloc_full();
    c->experiment_affinity = hwloc_bitmap_alloc_full();
#endif //HAVE_LIBHWLOC

    get_name_from_pid(client_pid, c->name, SCAF_MAX_CLIENT_NAME_LEN);
    HASH_ADD_INT(clients, pid, c);
}

void delete_client(scaf_client *c)
{
#ifdef HAVE_LIBHWLOC
    hwloc_bitmap_free(c->affinity);
    hwloc_bitmap_free(c->experiment_affinity);
#endif //HAVE_LIBHWLOC
    HASH_DEL(clients, c);
}

void text_print_clients(void)
{
    printf("%-9s%-9s%-8s%-8s%-15s%-5s%-10s%-10s%-6s%-9s\n", "PID", "NAME", "THREADS", "NLWP", "SECTION", "EFF", "CHECKINS", "MALLEABLE", "EXPT", "LASTSEEN");
    printf("%-9s%-9s%-8d%-8s%-15s%-5s%-10s%-10s%-6s%-9s\n", "all", "-", max_threads, "-", "-", "-", "-", "-", "-", "0");
    scaf_client *current, *tmp;
    HASH_ITER(hh, clients, current, tmp) {
        printf("%-9d%-9s%-8d%-8d%-15p%1.2f %-10u%-10s%-6s%-9f\n", current->pid, current->name, current->threads, get_nlwp(current->pid), current->current_section, current->metric, current->checkins, current->malleable?"YES":"NO", current->experimenting?"YES":"NO", rtclock()-current->last_checkin_time);
    }
    printf("\n");
    fflush(stdout);
}

int perform_client_request(scaf_client_message *client_message, scaf_daemon_message *daemon_message)
{
    int client_pid = client_message->pid;
    int client_request = client_message->message;

    float client_metric = client_message->message_value.efficiency;

    if(client_metric == 0.0)
        client_metric += 0.1;

    int client_threads;
    if(client_request == SCAF_NEW_CLIENT) {
        RW_LOCK_CLIENTS;
        int num_clients = HASH_COUNT(clients);
        client_threads = max_threads;
        add_client(client_pid, client_threads, client_message->section);
        UNLOCK_CLIENTS;
        pthread_kill(referee, SIGUSR1);

        daemon_message->num_clients = num_clients+1;
        daemon_message->threads = max_threads;
        return max_threads;
    } else if(client_request == SCAF_SECTION_START) {
        RW_LOCK_CLIENTS;
        int num_clients = HASH_COUNT(clients);
        scaf_client *client = find_client(client_pid);
        assert(client);
        client->current_section = client_message->section;
        client->metric = client_metric;
        client_threads = client->threads;
        client->checkins++;
        client->last_checkin_time = rtclock();
        UNLOCK_CLIENTS;
        daemon_message->num_clients = num_clients;
        daemon_message->threads = client_threads;
        return client_threads;
    } else if(client_request == SCAF_NOT_MALLEABLE) {
        RW_LOCK_CLIENTS;
        scaf_client *client = find_client(client_pid);
        assert(client);
        client->malleable = 0;
        UNLOCK_CLIENTS;
        //Wake referee again
        pthread_kill(referee, SIGUSR1);
        //num_clients is bogus here. We don't want to spent the time to
        //count the clients.
        daemon_message->num_clients = 0;
        daemon_message->threads = 0;
        return 0;
    } else if(client_request == SCAF_EXPT_START) {
        RW_LOCK_CLIENTS;
        scaf_client *client = find_client(client_pid);
        assert(client);
        client->experimenting = 1;
        client->experiment_pid = client_message->message_value.experiment_pid;
        UNLOCK_CLIENTS;
        //num_clients is bogus here. We don't want to spent the time to
        //count the clients.
        daemon_message->num_clients = 0;
        daemon_message->threads = 0;
        return 0;
    } else if(client_request == SCAF_EXPT_STOP) {
        RW_LOCK_CLIENTS;
        scaf_client *client = find_client(client_pid);
        assert(client);
        client->experimenting = 0;
        client->last_expt_time = rtclock();
        UNLOCK_CLIENTS;
        //num_clients is bogus here. We don't want to spent the time to
        //count the clients.
        daemon_message->num_clients = 0;
        daemon_message->threads = 0;
        return 0;
    } else if(client_request == SCAF_FORMER_CLIENT) {
        RW_LOCK_CLIENTS;
        scaf_client *old_client = find_client(client_pid);
        delete_client(old_client);
        int num_clients = HASH_COUNT(clients);
        UNLOCK_CLIENTS;
        free(old_client);
        daemon_message->num_clients = num_clients;
        daemon_message->threads = 0;
        pthread_kill(referee, SIGUSR1);
        return 0;
    }

    // Invalid request?
    return 0;
}

void maxspeedup_referee_body(void* data)
{
    int* intpart = malloc(0);
    float* floatpart = malloc(0);

    while(!stop_referee) {
        RW_LOCK_CLIENTS;
        scaf_client *current, *tmp;

        double now = rtclock();
        float metric_sum = 0.0;
        unsigned num_clients = HASH_COUNT(clients);
        unsigned num_experimenting = 0;
        int i;
        double time_since_expt = now - startuptime;

        {
            void *newpart;
            newpart = realloc(intpart, sizeof(int)*num_clients);
            assert(num_clients==0 || newpart != NULL);
            intpart = newpart;
            newpart = realloc(floatpart, sizeof(float)*num_clients);
            assert(num_clients==0 || newpart != NULL);
            floatpart = newpart;
        }

        HASH_ITER(hh, clients, current, tmp) {
            if(current->threads > 1)
                current->log_factor = max((current->metric * current->threads - 1.0),1.0)/log(current->threads);
            else
                current->log_factor = SERIAL_LOG_FACTOR;

            metric_sum += current->log_factor;
            time_since_expt = min(time_since_expt, now - current->last_expt_time);
            num_experimenting += (current->experimenting ? 1 : 0);
        }

        int available_threads = max_threads - ceil(bg_utilization - 0.5);
        available_threads = max(available_threads, 1);

        i=0;
        HASH_ITER(hh, clients, current, tmp) {
            float exact_ration = current->log_factor / metric_sum;
            floatpart[i++] = exact_ration;
        }

        if(experiment_equipartitioning && num_experimenting > 0 || time_since_expt < 2)
            intpart_equipartition_chunked(available_threads, intpart, chunksize, num_clients);
        else
            intpart_from_floatpart_chunked(available_threads, intpart, floatpart, chunksize, num_clients);

        i=0;
        HASH_ITER(hh, clients, current, tmp) {
            current->threads = intpart[i];
            i++;
        }

        UNLOCK_CLIENTS;

        apply_affinity_partitioning();

        usleep(REFEREE_PERIOD_US);
    }
    free(intpart);
    free(floatpart);
}

void equi_referee_body(void* data)
{
    int* intpart = malloc(0);

    while(!stop_referee) {
        RW_LOCK_CLIENTS;
        scaf_client *current, *tmp;

        int num_clients = HASH_COUNT(clients);

        int i=0;

        int available_threads = max_threads - ceil(bg_utilization - 0.5);

        {
            void *newpart;
            newpart = realloc(intpart, sizeof(int)*num_clients);
            assert(num_clients==0 || newpart != NULL);
            intpart = newpart;
        }

        if(eq_offset > 0 && num_clients == 2) {
            // Special case: if user specified number of threads for first client
            // of a two-client pair, then just force that configuration. This is a
            // silly option provided for development purposes.
            intpart[0] = eq_offset;
            intpart[1] = available_threads - eq_offset;
        } else {
            intpart_equipartition_chunked(available_threads, intpart, 1, num_clients);
        }

        i=0;
        HASH_ITER(hh, clients, current, tmp) {
            current->threads = intpart[i];
            i++;
        }

        UNLOCK_CLIENTS;

        apply_affinity_partitioning();

        usleep(REFEREE_PERIOD_US);
    }
    free(intpart);
}

void referee_switch_handler(int sig)
{

    // If we are the referee (not the master thread), do nothing. We are just
    // being woken from sleep for some reason.
    if(pthread_self() == referee) {
        signal(sig, referee_switch_handler);
        return;
    }

    void (*new_referee_body)(void *) = sig==SIGUSR1?&equi_referee_body:&maxspeedup_referee_body;
    if(text_interface)
        printf("Switching to %s referee!\n", sig==SIGUSR1?"equipartitioning":"maxspeedup");

    stop_referee = 1;
    pthread_join(referee, NULL);
    stop_referee = 0;
    pthread_create(&referee, NULL, (void *(*)(void*))new_referee_body, NULL);

    signal(sig, referee_switch_handler);
}

// Discrete IIR, single-pole lowpass filter. Used in scafd only by the referee
// using a priori profiling.  Time constant rc is expected to be the same
// across calls. Inputs x and dt are the data and time interval, respectively.
float lowpass(float x, float dt, float rc)
{
    static float yp = 0.5;
    float alpha = dt / (rc + dt);
    yp = alpha * x + (1.0-alpha) * yp;
    return yp;
}

void text_scoreboard_body(void* data)
{
    while(1) {
        RD_LOCK_CLIENTS;
        text_print_clients();
        UNLOCK_CLIENTS;
        sleep(text_interface);
    }
}

void reaper_body(void* data)
{
    while(1) {

        RD_LOCK_CLIENTS;
        scaf_client *current, *tmp;
        HASH_ITER(hh, clients, current, tmp) {
            // Check if a process is dead.
            if(kill(current->pid, 0)) {
                // If we actually found a dead process, then escalate and get a
                // write lock. This is assumed to happen infrequently!
                UNLOCK_CLIENTS;
                RW_LOCK_CLIENTS;
                delete_client(current);
                UNLOCK_CLIENTS;

                //When done, resume the read lock and free the allocation.
                pthread_kill(referee, SIGUSR1);
                free(current);
                RD_LOCK_CLIENTS;
            }
            //Sleep again for a bit.
            usleep(50000);
        }
        UNLOCK_CLIENTS;

        sleep(5);
    }
}

void lookout_body(void* data)
{
    // Here we poll several things at 1Hz, and take some actions if necessary.
    while(1) {
        // Just keep this global up to date. This has a built-in 1s sleep.
        bg_utilization = proc_get_cpus_used();

        // Also check if there are any processes that are not responsive. They
        // may be non-malleable, or malleable and very long-running.
        double now = rtclock();
        RW_LOCK_CLIENTS;
        scaf_client *current, *tmp;
        HASH_ITER(hh, clients, current, tmp) {
            if(now - current->last_checkin_time > unresponsive_threshold) {
                // This client has not been talking to us in a long time, but is
                // not dead. If it is not stopped (e.g., by job control) send it
                // SIGCONT to request feedback. This is a best-effort feature; we
                // shouldn't be too surprised if the signal doesn't get delivered
                // for some reason, or if the client cannot provide feedback for
                // some reason. SIGCONT is used because it is generally ignored by
                // default, so if a handler isn't in place for some reason it
                // shouldn't terminate anything.
                kill(current->pid, SIGCONT);
            }
        }
        UNLOCK_CLIENTS;
    }
}

int main(int argc, char **argv)
{

    int c;
    while( (c = getopt(argc, argv, "t:hebavu:E:C:T:x")) != -1) {
        switch(c) {
        case 'x':
            experiment_equipartitioning = 1;
            break;
        case 'e':
            equipartitioning = 1;
            break;
        case 't':
            text_interface = atoi(optarg);
            break;
        case 'u':
            if(atoi(optarg)>0)
                unresponsive_threshold = atoi(optarg);
            break;
        case 'b':
            nobgload = 1;
            break;
        case 'a':
            affinity = 0;
            break;
        case 'v':
            printf("scafd, %s\n%s\n", PACKAGE_STRING, PACKAGE_BUGREPORT);
            exit(1);
            break;
        case 'E':
            eq_offset = atoi(optarg);
            break;
        case 'C':
            if(atoi(optarg)>0)
                chunksize = atoi(optarg);
            break;
        case 'T':
            if(atoi(optarg)>0)
                max_threads = atoi(optarg);
            else
                max_threads = num_online_processors();
            if(max_threads < 1)
                max_threads = 1;
            break;
        case 'h':
        default:
            printf("scafd, %s\n%s\n", PACKAGE_STRING, PACKAGE_BUGREPORT);
            printf("\n");
            printf("Usage: %s [-h] [-b] %s[-e] [-t n] [-u n] [-C n] [-T n]\n"
                   "\t-h\tdisplay this message\n"
                   "\t-b\tdon't monitor background load: assume load of 0\n"
                   "%s"
                   "\t-e\tonly do strict equipartitioning\n"
                   "\t-t n\tuse a plain text status interface, printing every n seconds\n"
                   "\t-u n\tconsider a client unresponsive after n seconds. (default: %d)\n"
                   "\t-C n\tonly allocate threads in multiples of n. (default: 1)\n"
                   "\t-T n\tuse n threads for all processes. (default: %d)\n"
                   "\t-x\tdo equipartitioning during experiments. (default: disabled)\n",
                   argv[0],
                   affinity?"[-a] ":"",
                   affinity?"\t-a\tdisable affinity-based parallelism control\n":"",
                   (int)DEFAULT_UNRESPONSIVE_THRESHOLD,
                   num_online_processors());
            exit(1);
            break;
        }
    }

#ifdef HAVE_LIBHWLOC
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);
    num_hwloc_objs = hwloc_get_nbobjs_by_type(topology, part_at);
    top_obj = hwloc_get_root_obj(topology);

    apply_affinity_partitioning();
#endif

    bg_utilization = proc_get_cpus_used();
    startuptime = rtclock();

    if(max_threads == -1) {
        max_threads = num_online_processors();
    }
    if(chunksize == -1) {
        chunksize = 1;
    }
    assert(chunksize <= max_threads && "Impossible chunksize!");

    void (*referee_body)(void *) = equipartitioning?&equi_referee_body:&maxspeedup_referee_body;

    pthread_rwlock_init(&clients_lock, NULL);
    if(text_interface)
        pthread_create(&scoreboard, NULL, (void *(*)(void*))&text_scoreboard_body, NULL);
    pthread_create(&referee, NULL, (void *(*)(void*))referee_body, NULL);
    pthread_create(&reaper, NULL, (void *(*)(void*))&reaper_body, NULL);
    pthread_create(&lookout, NULL, (void *(*)(void*))&lookout_body, NULL);

    signal(SIGUSR1, referee_switch_handler);
    signal(SIGUSR2, referee_switch_handler);

    void *context = zmq_init (1);

    //  Socket to talk to clients
    void *responder = zmq_socket (context, ZMQ_REP);
    zmq_bind (responder, SCAF_CONNECT_STRING);

    while (1) {
        //  Wait for next request from client
        zmq_msg_t request;
        zmq_msg_init (&request);
#if ZMQ_VERSION_MAJOR > 2
        int r = zmq_recvmsg (responder, &request, 0);
#else
        int r = zmq_recv (responder, &request, 0);
#endif
        //  Ignore failed recvs for now; these are usually just interruptions
        //  due to SIGWINCH
        if(r<0)
            continue;

        scaf_client_message *client_message = (scaf_client_message*)(zmq_msg_data(&request));

        zmq_msg_t reply;
        zmq_msg_init_size (&reply, sizeof(scaf_daemon_message));
        scaf_daemon_message *dm = zmq_msg_data(&reply);
        dm->message = SCAF_DAEMON_FEEDBACK;

        // Update client bookkeeping if necessary
        int threads = perform_client_request(client_message, dm);
        assert(threads == dm->threads);
        assert(threads > 0 || client_message->message != SCAF_SECTION_START);
        assert(threads < 4096);
        zmq_msg_close (&request);

        //  Send reply back to client (even if the client doesn't care about an answer)
#if ZMQ_VERSION_MAJOR > 2
        zmq_sendmsg (responder, &reply, 0);
#else
        zmq_send (responder, &reply, 0);
#endif
        zmq_msg_close (&reply);
    }

    //  We never get here but if we did, this would be how we end
    zmq_close (responder);
    zmq_term (context);
    return 0;
}

