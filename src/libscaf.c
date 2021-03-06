//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2015
//
#include "../config.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>
#include <zmq.h>
#include "scaf.h"
#if(HAVE_LIBPAPI)
#include <papi.h>
#endif
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/reg.h>
#elif defined(__FreeBSD__)
#include <machine/vmm.h>
#endif

#include <sys/syscall.h>
#include <sys/wait.h>
#include <assert.h>
#include <pthread.h>

#if defined(__linux__)
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <sys/mman.h>

#if defined(__i386__)
#define ORIG_ACCUM (4 * ORIG_EAX)
#define ARGREG (4 * EBX)
#define ARG2REG (4 * ECX)
#define ARG3REG (4 * EDX)
#elif defined(__x86_64__)
#define ORIG_ACCUM (8 * ORIG_RAX)
#define ARGREG (8 * RDI)
#define ARG2REG (8 * RSI)
#define ARG3REG (8 * RDX)
#elif defined(__tilegx__)
// This is for TileGx, which is 64-bit. Guessing this stuff mostly.
#define ORIG_ACCUM (8 * TREG_SYSCALL_NR)
#define ARGREG (8 * 0)
// TODO: These two are just guesses. Verify.
#define ARG2REG (8 * 1)
#define ARG3REG (8 * 2)
#else
#error unsupported architecture
#endif
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/ptrace.h>
#include <machine/reg.h>
#else //__linux__
#error "Sadly, only Linux and FreeBSD are supported in this version of SCAF."
#endif //__linux__

#define SCAFD_TIMEOUT_SECONDS 1

// Experiment-related options
// Set this to 1 if parallel execution should wait for experiment to finish;
// otherwise it will be forced to terminate early.
#define SCAF_PARALLEL_WAIT_FOR_EXPERIMENT 0
// Set to 1 if SCAF should enforce a hard limit on the amount of time spent
// running a serial experiment.
#define SCAF_ENFORCE_EXPERIMENT_TIME_LIMIT 0
// The maximum amount of time that a experiment fork will run for.
#define SCAF_EXPERIMENT_TIME_LIMIT_SECONDS 10

// Limit measured efficiency to this value. This can restrict the effects of
// timing glitches resulting in crazy values.
#define SCAF_MEASURED_EFF_LIMIT 2.0

#define SCAF_LOWPASS_TIME_CONSTANT (4.0)

// PAPI high-level event to measure scalability by
//#define PAPI_HL_MEASURE PAPI_flops
//#define PAPI_HL_MEASURE PAPI_flips
#define PAPI_HL_MEASURE PAPI_ipc

static void *scaf_gomp_experiment_control(void *unused);
static scaf_client_experiment_description scaf_experiment_desc;
static inline void scaf_experiment_start(void);
static inline void scaf_experiment_end(int);
void scaf_advise_experiment_start(void);
void scaf_advise_experiment_stop(void);
static int inline scaf_will_create_experiment(void);

static int did_scaf_startup;
static void *scafd;
static void *scafd_context;

static zmq_msg_t *scaf_experiment_send_msg, *scaf_experiment_recv_msg;

static int scafd_available;
static int scaf_disable_experiments = 0;
static int scaf_experiment_process = 0;
volatile int scaf_experiment_starting = 0;

static int scaf_experiment_running = 0;
static int scaf_lazy_experiments;
static int scaf_mypid;
static int scaf_num_online_hardware_threads;
static int scaf_nullfd;
volatile int scaf_notified_not_malleable = 0;
static int scaf_explicitly_not_malleable = 0;
static int scaf_section_dumps = 0;
static FILE *scaf_sd;

static double scaf_init_rtclock;
static float scaf_section_duration;
static float scaf_section_ipc;
static float scaf_section_start_time;
static float scaf_section_start_process_time;
static float scaf_section_end_time;
static float scaf_section_end_process_time;
static float scaf_serial_duration;
static float scaf_section_efficiency;
static pthread_t scaf_master_thread;

// The minimum amount of time we'll run an experiment for. Amounts to a call to
// usleep, which can be expensive.
static useconds_t scaf_expt_min_useconds;

// For rate-limiting communications
// Maximum will be max_comms/per
static double scaf_rate_limit_per;
static double scaf_rate_limit_max_comms;
static double scaf_rate_limit_allowance;
static double scaf_rate_limit_last_check;
static int scaf_skip_communication_for_section = 0;

// For rate-limiting instrumentation efforts
// Maximum will be max_comms/per
static double scaf_math_rate_limit_per;
static double scaf_math_rate_limit_max_comms;
static double scaf_math_rate_limit_allowance;
static double scaf_math_rate_limit_last_check;
static int scaf_skip_math_for_section = 0;

static void *current_section_id;
static int current_threads;
static int current_num_clients = 1;
static scaf_client_section *current_section = NULL;
static scaf_client_section *sections = NULL;
static int scaf_in_parallel_section = 0;

#define LOWPASS_INITIAL (0.5)

// Discrete IIR, single-pole lowpass filter.
// Time constant rc is expected to be the same across calls. Inputs x and dt
// are the data and time interval, respectively.
#if defined(__GNUC__)
inline float lowpass(float x, float dt, float rc) {
#else
float lowpass(float x, float dt, float rc) {
#endif //__GNUC__
    static float yp = LOWPASS_INITIAL;
    float alpha = dt / (rc + dt);
    yp = alpha * x + (1.0 - alpha) * yp;
    return yp;
}

// Reset the lowpass filter to its initial state.
#if defined(__GNUC__)
inline float lowpass_reset(void) {
#else
float lowpass_reset(void) {
#endif //__GNUC__
    // By feeding the initial value back in with a zero time-constant, the
    // filter value is set to exactly the initial value.
    return lowpass(LOWPASS_INITIAL, 1.0, 0.0);
}

// Return 1 if we have been communicating too quickly. Implements a token
// bucket. Our convention will be that rate limiting is disabled if
// scaf_rate_limit_max_comms is negative.
static inline int scaf_communication_rate_limit(double current_time) {
    if(scaf_rate_limit_max_comms < 0)
        return 0;

    double time_passed = current_time - scaf_rate_limit_last_check;
    scaf_rate_limit_last_check = current_time;
    scaf_rate_limit_allowance +=
        time_passed * (scaf_rate_limit_max_comms / scaf_rate_limit_per);
    if(scaf_rate_limit_allowance > scaf_rate_limit_max_comms)
        scaf_rate_limit_allowance = scaf_rate_limit_max_comms;
    if(scaf_rate_limit_allowance < 1.0) {
        return 1; // do not allow the communication
    } else {
        scaf_rate_limit_allowance -= 1.0;
        return 0; // allow the communication.
    }
}

// Return 1 if we have been instrumenting too quickly/often. Implements a token
// bucket. Our convention will be that rate limiting is disabled if
// scaf_math_rate_limit_max_comms is negative.
static inline int scaf_math_rate_limit(double current_time) {
    if(scaf_math_rate_limit_max_comms < 0)
        return 0;

    double time_passed = current_time - scaf_math_rate_limit_last_check;
    scaf_math_rate_limit_last_check = current_time;
    scaf_math_rate_limit_allowance +=
        time_passed *
        (scaf_math_rate_limit_max_comms / scaf_math_rate_limit_per);
    if(scaf_math_rate_limit_allowance > scaf_math_rate_limit_max_comms)
        scaf_math_rate_limit_allowance = scaf_math_rate_limit_max_comms;
    if(scaf_math_rate_limit_allowance < 1.0) {
        return 1; // do not allow the communication
    } else {
        scaf_math_rate_limit_allowance -= 1.0;
        return 0; // allow the communication.
    }
}

static void scaf_feedback_requested(int sig) {
    // Only respond if we are in a parallel section. If for some reason the
    // experiment process gets this signal, do nothing.
    if(scaf_in_parallel_section && !scaf_experiment_process) {
        if(pthread_equal(scaf_master_thread, pthread_self())) {
            // Disable section dumps for these section start/stops since they
            // are
            // not in the code.
            int scaf_section_dumps_save = scaf_section_dumps;
            scaf_section_dumps = 0;
            // If we're the master thread, fake an empty serial section by
            // ending
            // the parallel section as far as bookkeeping is concerned.
            scaf_section_end();
            // Next, terminate any experiment and collect its results.
            scaf_gomp_experiment_destroy();
            // Finally, continue bookkeeping for the ongoing parallel section.
            scaf_section_start(current_section_id);
            // Re-enable scaf_section_dumps.
            scaf_section_dumps = scaf_section_dumps_save;
        } else {
            // If we're not the master thread, explicitly send the signal to the
            // master thread, but otherwise do nothing.
            pthread_kill(scaf_master_thread, sig);
        }
    }
}

// If possible, set up a signal handler for SIGCONT to interpret SIGCONT as a
// request to send an update to scafd. This is best-effort, and is allowed to
// fail. TODO: if there was already a handler, call it too.
static void scaf_init_signal_handler(void) {
    struct sigaction new_sa;
    new_sa.sa_handler = scaf_feedback_requested;
    new_sa.sa_flags = SA_RESTART;

    assert(0 == sigaction(SIGCONT, &new_sa, NULL));
}

inline static void scaf_dump_section_header(void) {
    fprintf(scaf_sd, "time, section, instance, event\n");
}
inline static void scaf_dump_section_start(void *section_id) {
    static unsigned instance = 0;
    fprintf(scaf_sd, "%3.6f, %p, %d, start\n", rtclock(), section_id,
            instance++);
}
inline static void scaf_dump_section_stop(void *section_id) {
    static unsigned instance = 0;
    fprintf(scaf_sd, "%3.6f, %p, %d, stop\n", rtclock(), section_id,
            instance++);
}

static void *scaf_init(void **context_p);
static int scaf_connect(void *scafd);
static scaf_client_section *scaf_add_client_section(void *section_id);
static scaf_client_section *scaf_find_client_section(void *section_id);
static void scaf_fork_prepare(void);
static void scaf_parent_postfork(void);

static scaf_client_section inline *scaf_add_client_section(void *section_id) {
    scaf_client_section *new_section = malloc(sizeof(scaf_client_section));
    new_section->section_id = section_id;
    new_section->last_time = 0;
    new_section->last_ipc = 1;
    new_section->experiment_complete = 0;
    new_section->experiment_serial_ipc = 0.5;
    HASH_ADD_PTR(sections, section_id, new_section);
    return new_section;
}

static scaf_client_section inline *scaf_find_client_section(void *section_id) {
    scaf_client_section *found = NULL;
    HASH_FIND_PTR(sections, &section_id, found);
    return found;
}

static int scaf_connect(void *scafd) {
    scafd_available = 1;
    zmq_pollitem_t pi;
    pi.socket = scafd;
    pi.events = ZMQ_POLLIN;

    assert(zmq_connect(scafd, SCAF_CONNECT_STRING) == 0);
    // send new client request and get initial num threads
    zmq_msg_t request;
    int retv = zmq_msg_init_size(&request, sizeof(scaf_client_message));
    assert(retv == 0);
    scaf_client_message *scaf_message =
        (scaf_client_message *)(zmq_msg_data(&request));
    scaf_message->message = SCAF_NEW_CLIENT;
    scaf_message->pid = scaf_mypid;
    scaf_message->section = current_section_id;
    zmq_sendmsg(scafd, &request, 0);
    zmq_msg_close(&request);

    // Stop and poll just to see if we timeout. If no reply, then assume there
    // is no scafd for the rest of execution.
    int rc = zmq_poll(&pi, 1, SCAFD_TIMEOUT_SECONDS * 1000);
    if(rc == 1) {
        zmq_msg_t reply;
        zmq_msg_init(&reply);
        zmq_recvmsg(scafd, &reply, 0);
        scaf_daemon_message response =
            *((scaf_daemon_message *)(zmq_msg_data(&reply)));
        assert(response.message == SCAF_DAEMON_FEEDBACK);
        zmq_msg_close(&reply);

        // Register scaf_retire() to be called upon exit. This just a courtesy
        // effort to notify scafd; if your platform for some reason never calls
        // this scafd will still figure out that you're gone on its own after a
        // few seconds.
        atexit(scaf_retire);

        scaf_num_online_hardware_threads = response.threads;

        // If a new client has arrived, reset our reported value so that
        // equipartitioning will be effected briefly.
        if(response.num_clients > current_num_clients)
            lowpass_reset();
        current_num_clients = response.num_clients;
        return response.threads;
    } else {
        // No response.
        scafd_available = 0;
        always_print(RED "WARNING: This SCAF client could not communicate with "
                         "scafd. Using %d threads." RESET "\n",
                     scaf_num_online_hardware_threads);
        return scaf_num_online_hardware_threads;
    }
}

static void *scaf_init(void **context_p) {
    scaf_mypid = getpid();
    scaf_nullfd = open("/dev/null", O_WRONLY | O_NONBLOCK);

    scaf_num_online_hardware_threads = scaf_get_num_cpus();

    scaf_init_signal_handler();

    // Set up fork handlers for this master thread.
    pthread_atfork(scaf_fork_prepare, scaf_parent_postfork, NULL);

    // Minimum experiment runtime
    char *minexpt = getenv("SCAF_EXPERIMENT_MIN_USECONDS");
    if(minexpt)
        scaf_expt_min_useconds = (useconds_t)atoll(minexpt);
    else
        scaf_expt_min_useconds = 100000;

    // Initialize stuff for rate limiting
    char *ratelimit = getenv("SCAF_COMM_RATE_LIMIT");
    if(ratelimit)
        scaf_rate_limit_max_comms = atof(ratelimit);
    else
        scaf_rate_limit_max_comms = 5.0;
    scaf_rate_limit_per = 1.0;
    scaf_rate_limit_allowance = scaf_rate_limit_max_comms;
    scaf_rate_limit_last_check = 0;

    // Math rate limiting
    char *mathratelimit = getenv("SCAF_MATH_RATE_LIMIT");
    if(mathratelimit)
        scaf_math_rate_limit_max_comms = atof(mathratelimit);
    else
        scaf_math_rate_limit_max_comms = 32.0;
    scaf_math_rate_limit_per = 1.0;
    scaf_math_rate_limit_allowance = scaf_math_rate_limit_max_comms;
    scaf_math_rate_limit_last_check = 0;

    // We used to call experiments "training." Obey the old environment variable
    // for now.
    char *noexperiments = getenv("SCAF_DISABLE_EXPERIMENTS");
    char *notraining = getenv("SCAF_DISABLE_TRAINING");
    if(noexperiments)
        scaf_disable_experiments = atoi(noexperiments);
    else if(notraining)
        scaf_disable_experiments = atoi(notraining);
    else
        scaf_disable_experiments = 0;

    char *lazyexperiments = getenv("SCAF_LAZY_EXPERIMENTS");
    if(lazyexperiments)
        scaf_lazy_experiments = atoi(lazyexperiments);
    else
        scaf_lazy_experiments = 1;

    char *sectiondumps = getenv("SCAF_SECTION_DUMPS");
    if(sectiondumps)
        scaf_section_dumps = atoi(sectiondumps);
    else
        scaf_section_dumps = 0;

    if(scaf_section_dumps) {
        char section_dump_filename[128];
        sprintf(section_dump_filename, "/tmp/scaf-sectiondump.%d.csv",
                scaf_mypid);
        scaf_sd = fopen(section_dump_filename, "w");
        scaf_dump_section_header();
    }

    // Check if the user is explicitly telling us that this process is not
    // malleable.
    char *user_not_malleable = getenv("SCAF_NOT_MALLEABLE");
    if(user_not_malleable)
        scaf_explicitly_not_malleable = atoi(user_not_malleable);
    else
        scaf_explicitly_not_malleable = 0;

    void *context = zmq_init(1);
    *context_p = context;
    void *requester = zmq_socket(context, ZMQ_REQ);
#if HAVE_LIBPAPI
    int initval = PAPI_library_init(PAPI_VER_CURRENT);
    PAPI_thread_init((unsigned long (*)(void))pthread_self);
    assert(initval == PAPI_VER_CURRENT || initval == PAPI_OK);
    // Do a test measurement.
    {
        float ipc, ptime;
        long long int ins;
        int ret = PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
        assert(ret == PAPI_OK);
    }
#endif
    scaf_master_thread = pthread_self();
    scaf_init_rtclock = rtclock();

    return requester;
}

void scaf_not_malleable(void) {
    // Don't report this over and over again.
    if(scaf_notified_not_malleable)
        return;

    // Only report once if in a parallel section.
    if(!pthread_equal(scaf_master_thread, pthread_self()))
        return;

    // This can be known before a parallel section starts. Initialize
    // communication with scafd if necessary.
    if(!did_scaf_startup) {
        scafd = scaf_init(&scafd_context);
        did_scaf_startup = 1;

        // scaf_connect gives a reply, but we just ignore it.
        scaf_connect(scafd);
    }

    if(!scafd_available)
        return;

    // send notice that we are not malleable.
    debug_print(BOLDRED "Notifying that we are NOT malleable." RESET "\n");
    zmq_msg_t request;
    zmq_msg_init_size(&request, sizeof(scaf_client_message));
    scaf_client_message *scaf_message =
        (scaf_client_message *)(zmq_msg_data(&request));
    scaf_message->message = SCAF_NOT_MALLEABLE;
    scaf_message->pid = scaf_mypid;
    zmq_sendmsg(scafd, &request, 0);
    zmq_msg_close(&request);

    zmq_msg_t reply;
    zmq_msg_init(&reply);
    zmq_recvmsg(scafd, &reply, 0);
    scaf_daemon_message response =
        *((scaf_daemon_message *)(zmq_msg_data(&reply)));
    assert(response.message == SCAF_DAEMON_FEEDBACK);
    zmq_msg_close(&reply);

    scaf_notified_not_malleable = 1;

    return;
}

void scaf_advise_experiment_start(void) {
    debug_print(BOLDRED "Notifying scafd of experiment start." RESET "\n");
    zmq_msg_t request;
    zmq_msg_init_size(&request, sizeof(scaf_client_message));
    scaf_client_message *scaf_message =
        (scaf_client_message *)(zmq_msg_data(&request));
    scaf_message->message = SCAF_EXPT_START;
    scaf_message->pid = scaf_mypid;
    scaf_message->message_value.experiment_pid =
        scaf_experiment_desc.experiment_pid;
    zmq_sendmsg(scafd, &request, 0);
    zmq_msg_close(&request);

    zmq_msg_t reply;
    zmq_msg_init(&reply);
    zmq_recvmsg(scafd, &reply, 0);
    scaf_daemon_message response =
        *((scaf_daemon_message *)(zmq_msg_data(&reply)));
    assert(response.message == SCAF_DAEMON_FEEDBACK);
    zmq_msg_close(&reply);
    return;
}

void scaf_advise_experiment_stop(void) {
    debug_print(BOLDRED "Notifying scafd of experiment stop." RESET "\n");
    zmq_msg_t request;
    zmq_msg_init_size(&request, sizeof(scaf_client_message));
    scaf_client_message *scaf_message =
        (scaf_client_message *)(zmq_msg_data(&request));
    scaf_message->message = SCAF_EXPT_STOP;
    scaf_message->pid = scaf_mypid;
    zmq_sendmsg(scafd, &request, 0);
    zmq_msg_close(&request);

    zmq_msg_t reply;
    zmq_msg_init(&reply);
    zmq_recvmsg(scafd, &reply, 0);
    scaf_daemon_message response =
        *((scaf_daemon_message *)(zmq_msg_data(&reply)));
    assert(response.message == SCAF_DAEMON_FEEDBACK);
    zmq_msg_close(&reply);
    return;
}

void scaf_retire(void) {
    // If we're just an experiment (with the atexit called before the fork),
    // then don't tell anyone anything.
    if(scaf_experiment_process)
        return;

    // send retire request
    zmq_msg_t request;
    zmq_msg_init_size(&request, sizeof(scaf_client_message));
    scaf_client_message *scaf_message =
        (scaf_client_message *)(zmq_msg_data(&request));
    scaf_message->message = SCAF_FORMER_CLIENT;
    scaf_message->pid = scaf_mypid;
    zmq_sendmsg(scafd, &request, 0);
    zmq_msg_close(&request);
    zmq_close(scafd);
    zmq_term(scafd_context);

    if(scaf_section_dumps)
        fclose(scaf_sd);

    return;
}

int scaf_section_start(void *section) {
    current_section_id = section;
    if(current_section == NULL || current_section->section_id != section) {
        current_section = scaf_find_client_section(current_section_id);
        if(current_section == NULL) {
            current_section = scaf_add_client_section(current_section_id);
        }

        if(!did_scaf_startup) {
            scafd = scaf_init(&scafd_context);
            did_scaf_startup = 1;

            // scaf_connect gives a reply, but we just ignore it.
            scaf_connect(scafd);
        }
    }

    if(scaf_explicitly_not_malleable)
        scaf_not_malleable();

    if(scaf_section_dumps)
        scaf_dump_section_start(current_section_id);

    if(!scafd_available) {
        current_threads = scaf_num_online_hardware_threads;
        scaf_in_parallel_section = 1;
        return scaf_num_online_hardware_threads;
    }

    if(current_threads < 1)
        current_threads = 1;
    if(scaf_section_duration <= 0.000001) {
        scaf_section_duration = 0.01;
        scaf_section_efficiency = 0.5;
    }
    if(scaf_serial_duration < 0.000000001) {
        scaf_serial_duration = 0.000000001;
    }

    // Compute results for reporting before this section
    float scaf_serial_efficiency = 1.0 / current_threads;
    float scaf_latest_efficiency_duration =
        (scaf_section_duration + scaf_serial_duration);
    float scaf_latest_efficiency =
        (scaf_section_efficiency * scaf_section_duration +
         scaf_serial_efficiency * scaf_serial_duration) /
        scaf_latest_efficiency_duration;
    float scaf_latest_efficiency_smooth =
        lowpass(scaf_latest_efficiency, scaf_latest_efficiency_duration,
                SCAF_LOWPASS_TIME_CONSTANT);

    // Based on the current time, decide whether or not to instrument this
    // parallel section. The idea is to avoid the overhead of setting up
    // hardware counters if we are entering/exiting parallel sections at a very
    // high rate. (The results for very short sections are less likely to be
    // useful anyway.) We will skip the instrumentation/math if we hit the rate
    // limiter or if experiments are disabled.
    double math_now = (float)(rtclock() - scaf_init_rtclock);
    scaf_skip_math_for_section =
        scaf_disable_experiments || scaf_math_rate_limit(math_now);

    if(scaf_skip_math_for_section) {
        scaf_section_start_time = math_now;
        scaf_serial_duration = scaf_section_start_time - scaf_section_end_time;
    } else {
// Collect data for future reporting
#if(HAVE_LIBPAPI)
        {
            float ipc, ptime;
            long long int ins;
            int ret =
                PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
            if(ret != PAPI_OK)
                always_print(
                    RED "WARNING: Bad PAPI things happening. (%s)" RESET "\n",
                    PAPI_strerror(ret));
            scaf_section_start_process_time = ptime;
            scaf_serial_duration =
                scaf_section_start_time - scaf_section_end_time;
        }
#else
        {
            scaf_section_start_time = (float)(rtclock() - scaf_init_rtclock);
            scaf_serial_duration =
                scaf_section_start_time - scaf_section_end_time;
        }
#endif
    }

    scaf_skip_communication_for_section =
        scaf_communication_rate_limit(scaf_section_start_time);
    if(!scaf_skip_communication_for_section) {
        // Get num threads
        zmq_msg_t request;
        zmq_msg_init_size(&request, sizeof(scaf_client_message));
        scaf_client_message *scaf_message =
            (scaf_client_message *)(zmq_msg_data(&request));
        scaf_message->message = SCAF_SECTION_START;
        scaf_message->pid = scaf_mypid;
        scaf_message->section = section;

        scaf_message->message_value.efficiency = scaf_latest_efficiency_smooth;

        zmq_sendmsg(scafd, &request, 0);
        zmq_msg_close(&request);

        zmq_msg_t reply;
        zmq_msg_init(&reply);
        zmq_recvmsg(scafd, &reply, 0);
        scaf_daemon_message response =
            *((scaf_daemon_message *)(zmq_msg_data(&reply)));
        assert(response.message == SCAF_DAEMON_FEEDBACK);
        zmq_msg_close(&reply);
        current_num_clients = response.num_clients;
        current_threads = response.threads;
    }

    scaf_section_ipc = 0.0;
    scaf_in_parallel_section = 1;

    if(scaf_notified_not_malleable)
        return scaf_num_online_hardware_threads;

    if(scaf_will_create_experiment()) {
        return current_threads - 1;
    }

    return current_threads;
}

void scaf_section_end(void) {

    if(scaf_section_dumps)
        scaf_dump_section_stop(current_section_id);

    if(!scafd_available) {
        scaf_in_parallel_section = 0;
        return;
    }

    if(scaf_skip_math_for_section) {
        scaf_section_ipc = current_section->last_ipc;
        scaf_section_end_time = (float)(float)(rtclock() - scaf_init_rtclock);
        scaf_section_duration =
            (scaf_section_end_time - scaf_section_start_time);
    } else {
#if(HAVE_LIBPAPI)
        {
            float ptime, ipc;
            long long int ins;
            int ret =
                PAPI_HL_MEASURE(&scaf_section_end_time, &ptime, &ins, &ipc);
            if(ret != PAPI_OK)
                always_print(
                    RED "WARNING: Bad PAPI things happening. (%s)" RESET "\n",
                    PAPI_strerror(ret));
            scaf_section_duration =
                (scaf_section_end_time - scaf_section_start_time);
            scaf_section_end_process_time = ptime;
            // The HL_MEASURE rate we get from PAPI is just for this thread,
            // while it
            // was running. This may not account for all of the wall time.
            // Estimate
            // the effective average rate over all threads by assuming that all
            // threads had similar rates while they were running, and 0
            // otherwise.
            if(!scaf_notified_not_malleable)
                ipc *= min(1.0, (scaf_section_end_process_time -
                                 scaf_section_start_process_time) /
                                    scaf_section_duration);

            scaf_section_ipc += ipc;
        }
#else
        {
            scaf_section_ipc += 0.5;
            scaf_section_end_time =
                (float)(float)(rtclock() - scaf_init_rtclock);
            scaf_section_duration =
                (scaf_section_end_time - scaf_section_start_time);
        }
#endif

        if(scaf_notified_not_malleable)
            scaf_section_ipc = scaf_section_ipc * current_threads;

        current_section->last_ipc = scaf_section_ipc;
    }

    current_section->last_time = scaf_section_duration;
    scaf_section_efficiency =
        min(SCAF_MEASURED_EFF_LIMIT,
            scaf_section_ipc / current_section->experiment_serial_ipc);
    debug_print(CYAN "Section (%p): @(%d){%f}{sIPC: %f; pIPC: %f} -> {EFF: %f; "
                     "SPU: %f}" RESET "\n",
                current_section->section_id, current_threads,
                scaf_section_duration, current_section->experiment_serial_ipc,
                scaf_section_ipc, scaf_section_efficiency,
                scaf_section_efficiency * current_threads);

    // If our "parallel" section was actually run on only 1 thread, also store
    // the results as the result of an experiment. (Even if an experiment had
    // already been run.)
    if(current_threads == 1 && !scaf_notified_not_malleable) {
        current_section->experiment_threads = 1;
        current_section->experiment_serial_ipc = scaf_section_ipc;
        current_section->experiment_parallel_ipc = scaf_section_ipc;
        current_section->experiment_ipc_eff = 1;
        current_section->experiment_ipc_speedup = 1;
        current_section->experiment_complete = 1;
    }

    scaf_in_parallel_section = 0;
    return;
}

// Allocate ZMQ structures or anything else that we might want to do before
// (outside of) the signal handler.
static inline void scaf_experiment_end_prep(void) {
    // Set up a new ZMQ context of our own. Note that we clobber the scafd
    // connection that we inherited from the control thread.
    void *context = zmq_init(1);
    scafd = zmq_socket(context, ZMQ_REQ);
    char parent_connect_string[64];
    sprintf(parent_connect_string, "ipc:///tmp/scaf-ipc-%d", scaf_mypid);
    assert(0 == zmq_connect(scafd, parent_connect_string));

    // Allocate the two messages which `scaf_experiment_end' will use.
    scaf_experiment_send_msg = malloc(sizeof(zmq_msg_t));
    scaf_experiment_recv_msg = malloc(sizeof(zmq_msg_t));

    // Initialize the messages with the actual message data to be used.
    assert(0 == zmq_msg_init_data(scaf_experiment_send_msg, &scaf_section_ipc,
                                  sizeof(float), NULL, NULL));
    // Re-use the same global pointer for the receive buffer if possible.
    // Otherwise, just allocate something new on the heap. (The compiler should
    // optimize away this branch entirely.)
    if(sizeof(float) >= sizeof(int)) {
        assert(0 == zmq_msg_init_data(scaf_experiment_recv_msg,
                                      &scaf_section_ipc, sizeof(int), NULL,
                                      NULL));
    } else {
        int *trash = malloc(sizeof(int));
        assert(0 == zmq_msg_init_data(scaf_experiment_recv_msg, trash,
                                      sizeof(int), NULL, NULL));
    }
}

static inline void scaf_experiment_start(void) {

#if(HAVE_LIBPAPI)
    {
        // Begin gathering information with PAPI.
        PAPI_shutdown();
        int initval = PAPI_library_init(PAPI_VER_CURRENT);
        PAPI_thread_init((unsigned long (*)(void))pthread_self);
        assert(initval == PAPI_VER_CURRENT || initval == PAPI_OK);

        float ipc, ptime;
        long long int ins;
        PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
        int ret = PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
        if(ret != PAPI_OK)
            always_print(RED "WARNING: Bad PAPI things happening. (%s)" RESET
                             "\n",
                         PAPI_strerror(ret));
        scaf_section_start_process_time = ptime;
    }
#else
    { scaf_section_start_time = 1.0; }
#endif

    // Initialize anything that will be needed in `scaf_experiment_end'. The
    // reason we do this ahead of time is because `scaf_experiment_end' will
    // potentially be reached as a signal handler, and we want to minimize the
    // number of dangerous things (e.g., allocs) we do in a signal handler. (On
    // SunOS we defer all of this until the signal handler because it's too
    // difficult to get the tracing facilities to cope with the ZMQ threads.)
    scaf_experiment_end_prep();

    struct sigaction new_sa;
    sigemptyset(&new_sa.sa_mask);
    sigaddset(&new_sa.sa_mask, SIGALRM);
    sigaddset(&new_sa.sa_mask, SIGINT);
    sigaddset(&new_sa.sa_mask, SIGCONT);
    new_sa.sa_handler = scaf_experiment_end;
    new_sa.sa_flags = 0;
    // Install the end of the experiment as the SIGINT handler.
    assert(0 == sigaction(SIGINT, &new_sa, NULL));
    // Also install the end of the experiment as the SIGALRM handler.
    assert(0 == sigaction(SIGALRM, &new_sa, NULL));

    if(SCAF_ENFORCE_EXPERIMENT_TIME_LIMIT)
        alarm(SCAF_EXPERIMENT_TIME_LIMIT_SECONDS);
}

static inline void scaf_experiment_end(int sig) {

    syscall(__NR_scaf_experiment_done);

    // Ignore all the signals which we might still get. (These are already
    // blocked if we arrived here as a signal handler, but not if we just called
    // it after finishing the experimental function.)
    if(sig == 0) {
        sigset_t sigs_end;
        sigemptyset(&sigs_end);
        sigaddset(&sigs_end, SIGALRM);
        sigaddset(&sigs_end, SIGINT);
        sigaddset(&sigs_end, SIGCONT);
        pthread_sigmask(SIG_BLOCK, &sigs_end, NULL);
    }

    debug_print(BLUE "SCAF experiment ending.");
    if(sig == SIGALRM) {
        debug_print(" (took too long.)\n");
    } else if(sig == 0) {
        debug_print(" (finished.)\n");
    } else if(sig == SIGINT) {
        debug_print(" (killed.)\n");
    } else {
        debug_print(" (not sure why?)\n");
    }
    debug_print(RESET);

#if(HAVE_LIBPAPI)
    {
        // Get the results from PAPI.
        float ipc, ptime;
        long long int ins;
        int ret = PAPI_HL_MEASURE(&scaf_section_end_time, &ptime, &ins, &ipc);
        if(ret != PAPI_OK)
            always_print(RED "WARNING: Bad PAPI things happening. (%s)" RESET
                             "\n",
                         PAPI_strerror(ret));
        scaf_section_duration =
            (scaf_section_end_time - scaf_section_start_time);
        scaf_section_end_process_time = ptime;
        // The HL_MEASURE rate we get from PAPI is just for this thread, while
        // it
        // was running. This may not account for all of the wall time. Estimate
        // the effective average rate over all threads by assuming that all
        // threads had similar rates while they were running, and 0 otherwise.
        if(!scaf_notified_not_malleable &&
           scaf_section_end_process_time != scaf_section_start_process_time)
            ipc = ipc * (scaf_section_end_process_time -
                         scaf_section_start_process_time) /
                  scaf_section_duration;

        scaf_section_ipc = ipc;
    }
#else
    {
        scaf_section_ipc = 1.0;
        scaf_section_duration = 1.0;
    }
#endif

    // The zmq messages here should already have been initialized outside of the
    // signal handler, including buffers.
    zmq_sendmsg(scafd, scaf_experiment_send_msg, 0);
    zmq_msg_close(scaf_experiment_send_msg);

    zmq_recvmsg(scafd, scaf_experiment_recv_msg, 0);
    zmq_msg_close(scaf_experiment_recv_msg);

    zmq_close(scafd);
    _Exit(0);
}

// Just decide whether or not we need to and can make an experiment.
static int inline scaf_will_create_experiment(void) {
    // First of all, only experiment if necessary.
    if(scaf_disable_experiments || !scafd_available ||
       current_section->experiment_complete)
        return 0;

    // Do not launch an experiment if we can only use one processor right now.
    if(current_threads < 2)
        return 0;

    // Also do not experiment if we are doing lazy experiments and there is no
    // multiprogramming right now.
    if(scaf_lazy_experiments && current_num_clients < 2)
        return 0;

#if(!HAVE_LIBPAPI)
    { return 0; }
#endif

    // Certain sections are banished due to bugs in SCAF. (E.g., can't get my
    // code to check if malloc is in the backtrace working on Solaris.)
    if(current_section->section_id == (void *)0x10002be40)
        return 0;

    return 1;
}

/* This is only used with the nanomsg version */
static void scaf_fork_prepare(void) { return; }

/* This is only used with the nanomsg version */
static void scaf_parent_postfork(void) { return; }

int scaf_gomp_experiment_create(void (*fn)(void *), void *data) {

    if(!scaf_will_create_experiment())
        return 0;

    scaf_experiment_starting = 1;
    scaf_experiment_desc.fn = fn;
    scaf_experiment_desc.data = data;
    pthread_barrier_init(&(scaf_experiment_desc.control_pthread_b), NULL, 2);
    pthread_create(&(scaf_experiment_desc.control_pthread), NULL,
                   &scaf_gomp_experiment_control, NULL);
    pthread_barrier_wait(&(scaf_experiment_desc.control_pthread_b));
    pthread_barrier_destroy(&(scaf_experiment_desc.control_pthread_b));
    scaf_experiment_starting = 0;
    scaf_experiment_running = 1;
    scaf_advise_experiment_start();

    return 1;
}

// An alias for the above.
int scaf_gomp_training_create(void (*fn)(void *), void *data) {
    return scaf_gomp_experiment_create(fn, data);
}

void scaf_gomp_experiment_destroy(void) {
    // Only perform experiment clean-up/collection if one was started.
    if(!scaf_experiment_running)
        return;

#if(!SCAF_PARALLEL_WAIT_FOR_EXPERIMENT)
    if(scaf_experiment_desc.experiment_pid != 0) {
        debug_print(GREEN "Killing child (%d) with SIGALRM." RESET "\n",
                    scaf_experiment_desc.experiment_pid);
        kill(scaf_experiment_desc.experiment_pid, SIGALRM);
    }
#endif

    // Mask/queue SIGCONT until we're done.
    sigset_t sigs_cont, sigs_old;
    sigemptyset(&sigs_cont);
    sigaddset(&sigs_cont, SIGCONT);
    pthread_sigmask(SIG_BLOCK, &sigs_cont, &sigs_old);

    // Get the experiment results from the child process.
    void *experiment_child = zmq_socket(scafd_context, ZMQ_REP);
    char child_connect_string[64];
    sprintf(child_connect_string, "ipc:///tmp/scaf-ipc-%d", scaf_mypid);
    assert(0 == zmq_bind(experiment_child, child_connect_string));
    zmq_msg_t reply;
    zmq_msg_init(&reply);
    zmq_recvmsg(experiment_child, &reply, 0);
    float response = *((float *)(zmq_msg_data(&reply)));
    zmq_msg_close(&reply);
    //  Send reply back to client (even if the client doesn't care about an
    //  answer)
    zmq_msg_init_size(&reply, sizeof(int));
    *((int *)(zmq_msg_data(&reply))) = 0;
    zmq_sendmsg(experiment_child, &reply, 0);
    zmq_msg_close(&reply);
    zmq_close(experiment_child);

    pthread_join(scaf_experiment_desc.control_pthread, NULL);
    current_section->experiment_threads = current_threads - 1;
    current_section->experiment_serial_ipc = response;
    current_section->experiment_parallel_ipc = scaf_section_ipc;
    current_section->experiment_ipc_eff = scaf_section_ipc / response;
    current_section->experiment_ipc_speedup =
        ((float)current_section->experiment_threads) *
        current_section->experiment_ipc_eff;
    current_section->experiment_complete = 1;
    debug_print(BLUE "Section (%p): @(1,%d){%f}{sIPC: %f; pIPC: %f} -> {EFF: "
                     "%f; SPU: %f}" RESET "\n",
                current_section->section_id,
                current_section->experiment_threads, scaf_section_duration,
                current_section->experiment_serial_ipc,
                current_section->experiment_parallel_ipc,
                current_section->experiment_ipc_eff,
                current_section->experiment_ipc_speedup);
    scaf_experiment_running = 0;
    scaf_advise_experiment_stop();

    // Restore signal handling.
    pthread_sigmask(SIG_SETMASK, &sigs_old, NULL);
}

// An alias for the above.
void scaf_gomp_training_destroy(void) { scaf_gomp_experiment_destroy(); }

// Report an unrecoverable error, kill the traced process and self.
static inline void scaf_abort_trace(const char *string, pid_t pid) {
    perror(string);
#if defined(__linux__)
    ptrace(PTRACE_KILL, pid, NULL, NULL);
#elif defined(__FreeBSD__)
    ptrace(PT_KILL, pid, NULL, 0);
#endif
    abort();
}

static void *scaf_gomp_experiment_control(void *unused) {
    void (*fn)(void *) = scaf_experiment_desc.fn;
    void *data = scaf_experiment_desc.data;
#if defined(__FreeBSD__)
    long lwp_id;
#endif //__FreeBSD__

    // Flush all file descriptors before forking. We can't have two copies of
    // buffered output going to file descriptors due to the fork.
    fflush(NULL);

    // Set up a set of signals referring to SIGINT and SIGALRM.
    sigset_t sigs_int_alrm, oldset;
    sigemptyset(&sigs_int_alrm);
    sigaddset(&sigs_int_alrm, SIGINT);
    sigaddset(&sigs_int_alrm, SIGALRM);
    // Block these signals until we're ready to handle them.
    pthread_sigmask(SIG_BLOCK, &sigs_int_alrm, &oldset);

    // Meet the other thread at the barrier. It won't start running the proper
    // instance of the section until we hit this barrier.
    pthread_barrier_wait(&(scaf_experiment_desc.control_pthread_b));

    int expPid = fork();
    if(expPid == 0) {
        scaf_experiment_desc.experiment_pid = getpid();
        // Note that experiment setup is finished.
        scaf_experiment_starting = 0;
        // Note that we are an experiment process.
        scaf_experiment_process = 1;
        // Start up our timing stuff with SCAF.
        scaf_experiment_start();

#if defined(__FreeBSD__)
        thr_self(&lwp_id);
        signal(SIGSYS, SIG_IGN);
#endif //__FreeBSD__

// Request that we be traced by the parent. The parent will be in charge of
// allowing/disallowing system calls, as well as killing us. Unnecessary in
// SunOS: we just issue a stop here and wait for the parent thread to run
// us again with tracing enabled.
#if defined(__linux__)
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
#elif defined(__FreeBSD__)
        ptrace(PT_TRACE_ME, 0, NULL, 0);
#endif
        kill(getpid(), SIGSTOP);

        // Unblock signals.
        sigprocmask(SIG_UNBLOCK, &sigs_int_alrm, NULL);
        // Run the parallel section in serial. The parent will intercept all
        // syscalls and ensure that we don't affect the state of the machine
        // incorrectly.
        fn(data);
        // When finished, send ourselves SIGINT to end the experiment.
        scaf_experiment_end(0);
        // If we get this far, just quit.
        _Exit(0);
    }

    scaf_experiment_desc.experiment_pid = expPid;
    // Restore signal handling.
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);

    if(expPid < 0) {
        perror("SCAF fork");
        exit(1);
    }

    int status;
#if defined(__FreeBSD__)
    long exp_lwp_id = 0;
#endif //__FreeBSD__

    if(waitpid(expPid, &status, 0) < 0) {
        perror("SCAF waitpid");
        abort();
    }

    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGSTOP);

    int foundRaW = 0;
    int foundW = 0;
    while(1) {

#if defined(__linux__)
        if(ptrace(PTRACE_SYSCALL, expPid, NULL, NULL) < 0)
#elif defined(__FreeBSD__)
        if(ptrace(PT_TO_SCE, expPid, (caddr_t)1, 0) < 0)
#endif
            scaf_abort_trace("SCAF ptrace(PTRACE_SYSCALL, ...)", expPid);

        if(waitpid(expPid, &status, 0) < 0) {
            scaf_abort_trace("SCAF waitpid", expPid);
        }

#if defined(__FreeBSD__)
        /* If we don't know the experiment LWP, grab it right out of memory. */
        if(!exp_lwp_id) {
            struct ptrace_io_desc iod;
            iod.piod_op = PIOD_READ_D;
            iod.piod_offs = &lwp_id;
            iod.piod_addr = &exp_lwp_id;
            iod.piod_len = sizeof(exp_lwp_id);
            int rv = ptrace(PT_IO, expPid, (caddr_t)&iod, 0);
            if(rv) {
                scaf_abort_trace("SCAF PIOD_READ_D", expPid);
            }
        }

        struct ptrace_lwpinfo lwpi;
        if(ptrace(PT_LWPINFO, expPid, (caddr_t)&lwpi, sizeof(lwpi))) {
            scaf_abort_trace("SCAF ptrace(PT_LWPINFO, ...)", expPid);
        }

        if(lwpi.pl_lwpid != exp_lwp_id) {
            /* This is the wrong LWP. Just let it keep going. */
            if(ptrace(PT_RESUME, lwpi.pl_lwpid, (caddr_t)1, 0)) {
                scaf_abort_trace("ptrace(PT_RESUME, ...)", expPid);
            }
            continue;
        }
#endif //__FreeBSD__

        int signal = WIFSTOPPED(status) ? WSTOPSIG(status) : -1;

        if(!WIFSTOPPED(status) || !WSTOPSIG(status) == SIGTRAP) {
#if defined(__linux__)
            ptrace(PTRACE_KILL, expPid, NULL, NULL);
#elif defined(__FreeBSD__)
            ptrace(PT_KILL, expPid, NULL, 0);
#endif
            break;
        }

        if(WSTOPSIG(status) == SIGALRM) {

            if(scaf_expt_min_useconds > 0) {
// The experiment has run long enough. We will stop it, but first let the
// function run for another small period of time. This is just an easy
// way to ensure that our experiment measurements have a minimum allowed
// runtime.
#if defined(__linux__)
                ptrace(PTRACE_CONT, expPid, NULL, 0);
#elif defined(__FreeBSD__)
                ptrace(PT_CONTINUE, expPid, NULL, 0);
#endif
                usleep(scaf_expt_min_useconds);

                kill(expPid, SIGALRM);
                waitpid(expPid, &status, 0);
            }

// The experiment has run long enough.
#if defined(__linux__)
            ptrace(PTRACE_DETACH, expPid, NULL, SIGALRM);
#elif defined(__FreeBSD__)
            ptrace(PT_DETACH, expPid, NULL, SIGALRM);
#endif
            break;
        }

#if defined(__linux__)
        int syscall = ptrace(PTRACE_PEEKUSER, expPid, ORIG_ACCUM, 0);
#elif defined(__FreeBSD__)
        struct reg regs;
        int rv = ptrace(PT_GETREGS, expPid, (caddr_t)&regs, 0);
        int syscall = rv < 0 ? rv : regs.r_rax;
#endif
        int foundUnsafeOpen = 0;

        if(syscall < 0 && signal == SIGSEGV) {
            always_print(BOLDRED "WARNING: experiment %d hit SIGSEGV." RESET
                                 "\n",
                         expPid);
// The experiment has segfaulted, so we'll have to just stop here.
// Deliver a SIGINT, continue the experiment, and detach. The experiment
// process will return from the bogus/noop syscall and go straight into
// the SIGINT signal handler.
#if defined(__linux__)
            ptrace(PTRACE_DETACH, expPid, NULL, SIGINT);
#elif defined(__FreeBSD__)
            ptrace(PT_DETACH, expPid, NULL, SIGINT);
#endif
            break;
        }
        assert(syscall >= 0);

        if(syscall == __NR_scaf_experiment_done) {
#if defined(__linux__)
            ptrace(PTRACE_DETACH, expPid, NULL, NULL);
#elif defined(__FreeBSD__)
            ptrace(PT_DETACH, expPid, NULL, 0);
#endif
            break;
        }

#if defined(__linux__)
        if(syscall == __NR_write) {
            // Replace the fd with one pointing to /dev/null. We'll keep track
            // of any
            // following reads to prevent violating RaW hazards through the
            // filesystem. (If necessary, we could do this more precisely by
            // tracking
            // RaWs per fd.)
            ptrace(PTRACE_POKEUSER, expPid, ARGREG, scaf_nullfd);
            foundW = 1;
        }
        if(syscall == __NR_read && foundW) {
            foundRaW = 1;
        }
#elif defined(__FreeBSD__)
        if(syscall == SYS_write || syscall == SYS_read) {
            // Too lazy to implement the one free write on FreeBSD. Just
            // report a hazard immediately.
            foundRaW = 1;
        }
#endif

// Some opens/mmaps are safe, depending on the arguments.
#if defined(__linux__)
#if defined(__tilegx__)
        if(syscall == __NR_openat) {
#else
        if(syscall == __NR_open) {
#endif //__tilegx__
            char *file = (char *)ptrace(PTRACE_PEEKUSER, expPid, ARGREG, 0);
            if(strcmp("/sys/devices/system/cpu/online", file) == 0) {
                // This is ok because it's always a read-only file.
            } else if(strcmp("/proc/stat", file) == 0) {
                // This is ok because it's always a read-only file.
            } else if(strcmp("/proc/meminfo", file) == 0) {
                // This is ok because it's always a read-only file.
            } else {
                debug_print(RED "Unsafe open: %s" RESET "\n", file);
                foundUnsafeOpen = 1;
            }
        } else if(syscall == __NR_mmap) {
            int prot = (int)ptrace(PTRACE_PEEKUSER, expPid, ARG3REG, 0);
            if(prot & MAP_PRIVATE) {
                // This is ok because changes won't go back to disk.
            } else if(prot & MAP_ANONYMOUS) {
                // This is ok because there is no associated file.
            } else {
                foundUnsafeOpen = 1;
            }
        }
#elif defined(__FreeBSD__)
        if(syscall == SYS_openat || syscall == SYS_open) {
            foundUnsafeOpen = 1;
        } else if(syscall == SYS_mmap) {
            foundUnsafeOpen = 1;
        }
#endif

#if defined(__linux__)
        if((syscall != __NR_rt_sigprocmask && syscall != __NR_rt_sigaction &&
            syscall != __NR_read && syscall != __NR_nanosleep &&
            syscall != __NR_write && syscall != __NR_restart_syscall &&
            syscall != __NR_mprotect && syscall != __NR_sched_getaffinity &&
            syscall != __NR_sched_setaffinity &&
#if defined(__tilegx__)
            syscall != __NR_openat &&
#else
            syscall != __NR_open &&
#endif //__tilegx__
            syscall != __NR_close && syscall != __NR_mmap &&
            syscall != __NR_fstat && syscall != __NR_munmap) ||
           foundRaW || foundUnsafeOpen) {
#elif defined(__FreeBSD__)
        if((syscall != SYS_sigprocmask && syscall != SYS_sigaction &&
            syscall != SYS_read && syscall != SYS_nanosleep &&
            syscall != SYS_write && syscall != SYS_mprotect &&
            syscall != SYS_cpuset_getaffinity &&
            syscall != SYS_cpuset_setaffinity && syscall != SYS_openat &&
            syscall != SYS_open && syscall != SYS_close &&
            syscall != SYS_mmap && syscall != SYS_fstat &&
            syscall != SYS_munmap &&
            syscall != SYS_syscall /* TODO: No idea what this is just yet! */
            ) ||
           foundRaW || foundUnsafeOpen) {
#endif //__linux__
            // This is not one of the syscalls deemed ``safe''. (Its completion
            // by
            // the kernel may affect the correctness of the program.) We must
            // stop
            // the experiment fork now.
            debug_print(RED "Parent: child (%d) has behaved badly (section %p, "
                            "syscall %d). Stopping it. (parent=%d)" RESET "\n",
                        expPid, current_section_id, syscall, getpid());

#if defined(__linux__)
            void *badCall = (void *)0xbadCa11;
            if(ptrace(PTRACE_POKEUSER, expPid, ORIG_ACCUM, badCall) < 0) {
                scaf_abort_trace("SCAF ptrace(PTRACE_POKEUSER, ...)", expPid);
            }
#elif defined(__FreeBSD__)
            // Change the syscall to an illegal number. This will
            // result in SIGSYS, which we can ignore. This is the
            // easiest way I can find to skip a syscall on FreeBSD.
            regs.r_rax = SYS_MAXSYSCALL;
            if(ptrace(PT_SETREGS, expPid, (caddr_t)&regs, 0)) {
                scaf_abort_trace("SCAF ptrace(PT_SETREGS, ...)", expPid);
            }
#endif

// Deliver a SIGINT, continue the experiment, and detach. The experiment
// process will return from the bogus/noop syscall and go straight into
// the SIGINT signal handler.
#if defined(__linux__)
            ptrace(PTRACE_DETACH, expPid, NULL, SIGINT);
#elif defined(__FreeBSD__)
            ptrace(PT_DETACH, expPid, (caddr_t)1, SIGINT);
#endif
            break;
        }

#if defined(__FreeBSD__)
        // FreeBSD needs us to explicitly send the thread on its
        // way again.
        if(ptrace(PT_RESUME, exp_lwp_id, (caddr_t)1, 0)) {
            scaf_abort_trace("SCAF ptrace(PT_RESUME, ...)", expPid);
        }
#endif //__FreeBSD__

    } // while(1)

    // We will always have killed the child by now.
    waitpid(expPid, &status, 0);
    return NULL;
}
