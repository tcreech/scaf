//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2012
//
#include "../config.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>
#include <zmq.h>
#if defined(__KNC__)
int omp_get_num_threads(void);
int omp_get_max_threads(void);
#else
#include <omp.h>
#endif //defined(__KNC__)
#include "scaf.h"
#if(HAVE_LIBPAPI)
#include <papi.h>
#endif
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/reg.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <assert.h>
#include <pthread.h>

#if defined(__linux__)
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <sys/mman.h>

#if defined(__i386__)
#define ORIG_ACCUM	(4 * ORIG_EAX)
#define ARGREG	(4 * EBX)
#define ARG2REG	(4 * ECX)
#define ARG3REG	(4 * EDX)
#elif defined(__x86_64__)
#define ORIG_ACCUM	(8 * ORIG_RAX)
#define ARGREG	(8 * RDI)
#define ARG2REG	(8 * RSI)
#define ARG3REG	(8 * RDX)
#elif defined(__tilegx__)
// This is for TileGx, which is 64-bit. Guessing this stuff mostly.
#define ORIG_ACCUM   (8 * TREG_SYSCALL_NR)
#define ARGREG      (8 * 0)
//TODO: These two are just guesses. Verify.
#define ARG2REG      (8 * 1)
#define ARG3REG      (8 * 2)
#else
#error unsupported architecture
#endif

#endif //__linux__

#if defined(__sun)
#include "solaris_trace_utils.h"
// Solaris has actual APIs for tracing syscalls and peeking at arguments, so
// none of the above hackery required for Linux is necessary.
#endif //__sun

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

#if defined(__sun)
#define SCAF_LOWPASS_TIME_CONSTANT (15.0)
#elif defined(__KNC__)
#define SCAF_LOWPASS_TIME_CONSTANT (40.0)
#else //__sun
#define SCAF_LOWPASS_TIME_CONSTANT (2.0)
#endif //__sun

// PAPI high-level event to measure scalability by
//#define PAPI_HL_MEASURE PAPI_flops
//#define PAPI_HL_MEASURE PAPI_flips
#define PAPI_HL_MEASURE PAPI_ipc

static void* scaf_gomp_experiment_control(void *unused);
static scaf_client_experiment_description scaf_experiment_desc;
static inline void scaf_experiment_start(void);
static inline void scaf_experiment_end(int);

static int did_scaf_startup;
static void *scafd;
static void *scafd_context;

static int scafd_available;
static int scaf_disable_experiments = 0;
static int scaf_enable_firsttouch = 0;
static int scaf_experiment_process = 0;
static int scaf_lazy_experiments;
static int scaf_mypid;
static int omp_max_threads;
static int scaf_nullfd;

static double scaf_init_rtclock;
static float scaf_section_duration;
static float scaf_section_ipc;
static float scaf_section_start_time;
static float scaf_section_end_time;
static float scaf_serial_duration;
static float scaf_section_efficiency;
static pthread_t scaf_master_thread;

// For rate-limiting communications
// Maximum will be max_comms/per
static double scaf_rate_limit_per;
static double scaf_rate_limit_max_comms;
static double scaf_rate_limit_allowance;
static double scaf_rate_limit_last_check;
static int scaf_skip_communication_for_section  = 0;

static void* current_section_id;
static int current_threads;
static int current_num_clients = 1;
static scaf_client_section *current_section = NULL;
static scaf_client_section *sections = NULL;

// Discrete IIR, single-pole lowpass filter.
// Time constant rc is expected to be the same across calls. Inputs x and dt
// are the data and time interval, respectively.
#if defined(__GNUC__)
inline float lowpass(float x, float dt, float rc){
#else
float lowpass(float x, float dt, float rc){
#endif //__GNUC__
   static float yp = 0.5;
   float alpha = dt / (rc + dt);
   yp = alpha * x + (1.0-alpha) * yp;
   return yp;
}

// Return 1 if we have been communicating too quickly. Implements a token
// bucket. Our convention will be that rate limiting is disabled if
// scaf_rate_limit_max_comms is negative.
static inline int scaf_communication_rate_limit(double current_time){
   if(scaf_rate_limit_max_comms < 0)
      return 0;

   double time_passed = current_time - scaf_rate_limit_last_check;
   scaf_rate_limit_last_check = current_time;
   scaf_rate_limit_allowance += time_passed * (scaf_rate_limit_max_comms / scaf_rate_limit_per);
   if(scaf_rate_limit_allowance > scaf_rate_limit_max_comms)
      scaf_rate_limit_allowance = scaf_rate_limit_max_comms;
   if(scaf_rate_limit_allowance < 1.0){
      return 1; // do not allow the communication
   }
   else{
      scaf_rate_limit_allowance -= 1.0;
      return 0; // allow the communication.
   }
}

static void* scaf_init(void **context_p);
static int scaf_connect(void *scafd);
static scaf_client_section*  scaf_add_client_section(void *section_id);
static scaf_client_section* scaf_find_client_section(void *section_id);

static scaf_client_section inline *scaf_add_client_section(void *section_id){
   scaf_client_section *new_section = malloc(sizeof(scaf_client_section));
   new_section->section_id = section_id;
   new_section->last_time = 0;
   new_section->last_ipc = 1;
   new_section->experiment_complete = 0;
   new_section->first_touch_complete = !scaf_enable_firsttouch;
   new_section->experiment_serial_ipc = 0.5;
   HASH_ADD_PTR(sections, section_id, new_section);
   return new_section;
}

static scaf_client_section inline *scaf_find_client_section(void *section_id){
   scaf_client_section *found = NULL;
   HASH_FIND_PTR(sections, &section_id, found);
   return found;
}

static int scaf_connect(void *scafd){
   scafd_available = 1;
   zmq_pollitem_t pi;
   pi.socket = scafd;
   pi.events = ZMQ_POLLIN;

   assert(zmq_connect(scafd, SCAF_CONNECT_STRING) == 0);
   // send new client request and get initial num threads
   zmq_msg_t request;
   int retv = zmq_msg_init_size(&request, sizeof(scaf_client_message));
   assert(retv==0);
   scaf_client_message *scaf_message = (scaf_client_message*)(zmq_msg_data(&request));
   scaf_message->message = SCAF_NEW_CLIENT;
   scaf_message->pid = scaf_mypid;
   scaf_message->section = current_section_id;
#if ZMQ_VERSION_MAJOR > 2
   zmq_sendmsg(scafd, &request, 0);
#else
   zmq_send(scafd, &request, 0);
#endif
   zmq_msg_close(&request);

   // Stop and poll just to see if we timeout. If no reply, then assume there
   // is no scafd for the rest of execution.
#if ZMQ_VERSION_MAJOR > 2
   int rc = zmq_poll(&pi, 1, SCAFD_TIMEOUT_SECONDS*1000);
#else
   int rc = zmq_poll(&pi, 1, SCAFD_TIMEOUT_SECONDS*1000000);
#endif
   if(rc == 1){
      zmq_msg_t reply;
      zmq_msg_init(&reply);
#if ZMQ_VERSION_MAJOR > 2
      zmq_recvmsg(scafd, &reply, 0);
#else
      zmq_recv(scafd, &reply, 0);
#endif
      scaf_daemon_message response = *((scaf_daemon_message*)(zmq_msg_data(&reply)));
      assert(response.message == SCAF_DAEMON_FEEDBACK);
      zmq_msg_close(&reply);

      // Register scaf_retire() to be called upon exit. This just a courtesy
      // effort to notify scafd; if your platform for some reason never calls
      // this scafd will still figure out that you're gone on its own after a
      // few seconds.
      atexit(scaf_retire);

      current_num_clients = response.num_clients;
      return response.threads;
   } else {
      // No response.
      scafd_available = 0;
      omp_max_threads = omp_get_max_threads();
      always_print(RED "WARNING: This SCAF client could not communicate with scafd." RESET "\n");
      return omp_max_threads;
   }
}


static void* scaf_init(void **context_p){
   scaf_mypid = getpid();
   scaf_init_rtclock = rtclock();
   scaf_nullfd = open("/dev/null", O_WRONLY | O_NONBLOCK);

   // Initialize stuff for rate limiting
   char *ratelimit = getenv("SCAF_COMM_RATE_LIMIT");
   if(ratelimit)
      scaf_rate_limit_max_comms = atof(ratelimit);
   else
      scaf_rate_limit_max_comms = 100.0;
   scaf_rate_limit_per = 1.0;
   scaf_rate_limit_allowance = scaf_rate_limit_max_comms;
   scaf_rate_limit_last_check = 0;

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

   char *firsttouch = getenv("SCAF_ENABLE_FIRSTTOUCH");
   if(firsttouch)
     scaf_enable_firsttouch = atoi(firsttouch);
   else
     scaf_enable_firsttouch = 0;

   void *context = zmq_init(1);
   *context_p = context;
   void *requester = zmq_socket (context, ZMQ_REQ);
#if HAVE_LIBPAPI
   PAPI_thread_init((unsigned long (*)(void) )pthread_self);
   int initval = PAPI_library_init(PAPI_VER_CURRENT);
   assert(initval == PAPI_VER_CURRENT || initval == PAPI_OK);
   assert(PAPI_multiplex_init() == PAPI_OK);
#endif
   scaf_master_thread = pthread_self();

   return requester;
}

void scaf_retire(void){
   // If we're just an experiment (with the atexit called before the fork),
   // then don't tell anyone anything.
   if(scaf_experiment_process)
      return;

   // send retire request
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(scaf_client_message));
   scaf_client_message *scaf_message = (scaf_client_message*)(zmq_msg_data(&request));
   scaf_message->message = SCAF_FORMER_CLIENT;
   scaf_message->pid = scaf_mypid;
#if ZMQ_VERSION_MAJOR > 2
   zmq_sendmsg(scafd, &request, 0);
#else
   zmq_send(scafd, &request, 0);
#endif
   zmq_msg_close(&request);
   zmq_close (scafd);
   zmq_term (scafd_context);
   return;
}

int scaf_section_start(void* section){
   current_section_id = section;
   if(current_section == NULL || current_section->section_id != section){
      current_section = scaf_find_client_section(current_section_id);
      if(current_section == NULL){
         current_section = scaf_add_client_section(current_section_id);
      }

      if(unlikely(!did_scaf_startup)){
         scafd = scaf_init(&scafd_context);
         did_scaf_startup=1;

         // scaf_connect gives a reply, but we just ignore it.
         scaf_connect(scafd);
      }
   }

   if(unlikely(!scafd_available)){
      current_threads = omp_max_threads;
      return omp_max_threads;
   }

   if(unlikely(current_threads < 1))
      current_threads=1;
   if(unlikely(scaf_section_duration <= 0.000001)){
      scaf_section_duration = 0.01;
      scaf_section_efficiency = 0.5;
   }

   // Compute results for reporting before this section
   float scaf_serial_efficiency = 1.0 / current_threads;
   float scaf_latest_efficiency_duration = (scaf_section_duration + scaf_serial_duration);
   float scaf_latest_efficiency = (scaf_section_efficiency * scaf_section_duration + scaf_serial_efficiency * scaf_serial_duration) / scaf_latest_efficiency_duration;
   float scaf_latest_efficiency_smooth = lowpass(scaf_latest_efficiency, scaf_latest_efficiency_duration, SCAF_LOWPASS_TIME_CONSTANT);

   // Collect data for future reporting
#if(HAVE_LIBPAPI)
   {
      float ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
      scaf_serial_duration = scaf_section_start_time - scaf_section_end_time;
      if(unlikely(ret != PAPI_OK)) always_print(RED "WARNING: Bad PAPI things happening. (%d)" RESET "\n", ret);
   }
#else
   {
      scaf_section_start_time = (float)(rtclock() - scaf_init_rtclock);
      scaf_serial_duration = scaf_section_start_time - scaf_section_end_time;
   }
#endif

   scaf_skip_communication_for_section = scaf_communication_rate_limit(scaf_section_start_time);
   if(!scaf_skip_communication_for_section){
      // Get num threads
      zmq_msg_t request;
      zmq_msg_init_size(&request, sizeof(scaf_client_message));
      scaf_client_message *scaf_message = (scaf_client_message*)(zmq_msg_data(&request));
      scaf_message->message = SCAF_SECTION_START;
      scaf_message->pid = scaf_mypid;
      scaf_message->section = section;

      scaf_message->efficiency = scaf_latest_efficiency_smooth;

#if ZMQ_VERSION_MAJOR > 2
      zmq_sendmsg(scafd, &request, 0);
#else
      zmq_send(scafd, &request, 0);
#endif
      zmq_msg_close(&request);

      zmq_msg_t reply;
      zmq_msg_init(&reply);
#if ZMQ_VERSION_MAJOR > 2
      zmq_recvmsg(scafd, &reply, 0);
#else
      zmq_recv(scafd, &reply, 0);
#endif
      scaf_daemon_message response = *((scaf_daemon_message*)(zmq_msg_data(&reply)));
      assert(response.message == SCAF_DAEMON_FEEDBACK);
      zmq_msg_close(&reply);
      current_num_clients = response.num_clients;
      current_threads = response.threads;
   }

   scaf_section_ipc = 0.0;

#if(HAVE_LIBPAPI)
   if(unlikely(!current_section->experiment_complete && current_section->first_touch_complete && scafd_available && !scaf_disable_experiments && !(scaf_lazy_experiments && current_num_clients < 2)))
#if defined(__KNC__)
      return current_threads-4;
#else
      return current_threads-1;
#endif //defined(__KNC__)
#endif

   return current_threads;
}

void scaf_section_end(void){

   if(unlikely(!scafd_available))
      return;

#if(HAVE_LIBPAPI)
   {
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&rtime, &ptime, &ins, &ipc);
      if(unlikely(ret != PAPI_OK)) always_print(RED "WARNING: Bad PAPI things happening. (%d)" RESET "\n", ret);
      scaf_section_ipc += ipc;
      scaf_section_end_time = rtime;
      scaf_section_duration = (scaf_section_end_time - scaf_section_start_time);
   }
#else
   {
      scaf_section_ipc += 0.5;
      scaf_section_end_time = (float)(float)(rtclock() - scaf_init_rtclock);
      scaf_section_duration = (scaf_section_end_time - scaf_section_start_time);
   }
#endif

   current_section->last_time = scaf_section_duration;
   current_section->last_ipc  = scaf_section_ipc;
   scaf_section_efficiency = min(SCAF_MEASURED_EFF_LIMIT, scaf_section_ipc / current_section->experiment_serial_ipc);

   return;
}

static inline void scaf_experiment_start(void){

#if(HAVE_LIBPAPI)
   {
      // Begin gathering information with PAPI.
      int initval = PAPI_library_init(PAPI_VER_CURRENT);
      assert(initval == PAPI_VER_CURRENT || initval == PAPI_OK);
      assert(PAPI_multiplex_init() == PAPI_OK);

      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&rtime, &ptime, &ins, &ipc);
      scaf_section_start_time = rtime;
      if(ret != PAPI_OK) always_print(RED "WARNING: Bad PAPI things happening. (%d)" RESET "\n", ret);
   }
#else
   {
      scaf_section_start_time = 1.0;
   }
#endif

   // Install the end of the experiment as the SIGINT handler.
   signal(SIGINT, scaf_experiment_end);
   // Also install the end of the experiment as the SIGALRM handler.
   signal(SIGALRM, scaf_experiment_end);

   if(SCAF_ENFORCE_EXPERIMENT_TIME_LIMIT)
      alarm(SCAF_EXPERIMENT_TIME_LIMIT_SECONDS);
}

static inline void scaf_experiment_end(int sig){

   syscall(__NR_scaf_experiment_done);

   // Ignore all the signals which we might still get.
   signal(SIGALRM, SIG_IGN);
   signal(SIGINT, SIG_IGN);

   debug_print(BLUE "SCAF experiment ending.");
   if(sig == SIGALRM){
     debug_print(" (took too long.)\n");
   }
   else if(sig == 0){
     debug_print(" (finished.)\n");
   }
   else if(sig == SIGINT){
     debug_print(" (killed.)\n");
   }
   else {
     debug_print(" (not sure why?)\n");
   }
   debug_print(RESET);

#if(HAVE_LIBPAPI)
   {
      // Get the results from PAPI.
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&rtime, &ptime, &ins, &ipc);
      if(ret != PAPI_OK) always_print(RED "WARNING: Bad PAPI things happening. (%d)" RESET "\n", ret);
      scaf_section_ipc = ipc;
      scaf_section_end_time = rtime;
      scaf_section_duration = (scaf_section_end_time - scaf_section_start_time);
   }
#else
   {
      scaf_section_ipc = 1.0;
      scaf_section_duration = 1.0;
   }
#endif

   void *context = zmq_init(1);
   scafd = zmq_socket (context, ZMQ_REQ);
   char parent_connect_string[64];
   sprintf(parent_connect_string, "ipc:///tmp/scaf-ipc-%d", scaf_mypid);
   assert(0==zmq_connect(scafd, parent_connect_string));
   zmq_msg_t request;
   assert(0==zmq_msg_init_data(&request, &scaf_section_ipc, sizeof(float), NULL, NULL));
   
#if ZMQ_VERSION_MAJOR > 2
   zmq_sendmsg(scafd, &request, 0);
#else
   zmq_send(scafd, &request, 0);
#endif
   zmq_msg_close(&request);

   zmq_msg_t reply;
   zmq_msg_init(&reply);
#if ZMQ_VERSION_MAJOR > 2
      zmq_recvmsg(scafd, &reply, 0);
#else
      zmq_recv(scafd, &reply, 0);
#endif
   //int response = *((int*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);

   zmq_close(scafd);
   _Exit(0);
}

int scaf_gomp_experiment_create(void (*fn) (void*), void *data){
   // First of all, only experiment if necessary.
   if(scaf_disable_experiments || !scafd_available || current_section->experiment_complete || !(current_threads>1))
      return 0;

   // Also do not experiment if we are doing lazy experiments and there is no
   // multiprogramming right now.
   if(scaf_lazy_experiments && current_num_clients < 2)
      return 0;

#if(! HAVE_LIBPAPI)
   {
      return 0;
   }
#endif

   if(!current_section->first_touch_complete)
      return 0;

   scaf_experiment_desc.fn = fn;
   scaf_experiment_desc.data = data;
   pthread_barrier_init(&(scaf_experiment_desc.control_pthread_b), NULL, 2);
   pthread_create(&(scaf_experiment_desc.control_pthread), NULL, &scaf_gomp_experiment_control, NULL);
   pthread_barrier_wait(&(scaf_experiment_desc.control_pthread_b));
   pthread_barrier_destroy(&(scaf_experiment_desc.control_pthread_b));
   return 1;
}

// An alias for the above.
int scaf_gomp_training_create(void (*fn) (void*), void *data){
   return scaf_gomp_experiment_create(fn, data);
}

void scaf_gomp_experiment_destroy(void){
   // First of all, only experiment if necessary.
   if(scaf_disable_experiments || !scafd_available || current_section->experiment_complete || !(current_threads>1))
      return;

   // Also do not experiment if we are doing lazy experiments and there is no
   // multiprogramming right now.
   if(scaf_lazy_experiments && current_num_clients < 2)
      return;

#if(! HAVE_LIBPAPI)
   {
      return;
   }
#endif

   if(!current_section->first_touch_complete){
      current_section->first_touch_complete = 1;
      return;
   }

#if(! SCAF_PARALLEL_WAIT_FOR_EXPERIMENT)
   {
      kill(scaf_experiment_desc.experiment_pid, SIGALRM);
   }
#endif

   // Get the experiment results from the child process.
   void *experiment_child = zmq_socket(scafd_context, ZMQ_REP);
   char child_connect_string[64];
   sprintf(child_connect_string, "ipc:///tmp/scaf-ipc-%d", scaf_mypid);
   assert(0==zmq_bind(experiment_child, child_connect_string));
   zmq_msg_t reply;
   zmq_msg_init(&reply);
#if ZMQ_VERSION_MAJOR > 2
      zmq_recvmsg(experiment_child, &reply, 0);
#else
      zmq_recv(experiment_child, &reply, 0);
#endif
   float response = *((float*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);
   //  Send reply back to client (even if the client doesn't care about an answer)
   zmq_msg_init_size (&reply, sizeof(int));
   *((int*)(zmq_msg_data(&reply))) = 0;
#if ZMQ_VERSION_MAJOR > 2
   zmq_sendmsg (experiment_child, &reply, 0);
#else
   zmq_send (experiment_child, &reply, 0);
#endif
   zmq_msg_close (&reply);
   zmq_close(experiment_child);

   pthread_join(scaf_experiment_desc.control_pthread, NULL);
   current_section->experiment_threads = current_threads-1;
   current_section->experiment_serial_ipc = response;
   current_section->experiment_parallel_ipc = scaf_section_ipc;
   current_section->experiment_ipc_eff = scaf_section_ipc / response;
   current_section->experiment_ipc_speedup = ((float)current_section->experiment_threads) * current_section->experiment_ipc_eff;
   current_section->experiment_complete = 1;
   debug_print(BLUE "Section (%p): @(1,%d){%f}{sIPC: %f; pIPC: %f} -> {EFF: %f; SPU: %f}" RESET "\n", current_section->section_id, current_section->experiment_threads, scaf_section_duration, current_section->experiment_serial_ipc, current_section->experiment_parallel_ipc, current_section->experiment_ipc_eff, current_section->experiment_ipc_speedup);
}

// An alias for the above.
void scaf_gomp_training_destroy(void){
   scaf_gomp_experiment_destroy();
}

static void* scaf_gomp_experiment_control(void *unused){
   void (*fn) (void*) = scaf_experiment_desc.fn;
   void *data = scaf_experiment_desc.data;

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

  int expPid = fork();
  scaf_experiment_desc.experiment_pid = expPid;
  if(expPid==0){
    // Note that we are an experiment process.
    scaf_experiment_process = 1;
    // Put ourselves in the parent's process group.
    setpgid(0, scaf_mypid);
    // Start up our timing stuff with SCAF.
    scaf_experiment_start();


#if defined(__linux__)
    // Request that we be traced by the parent. The parent will be in charge of
    // allowing/disallowing system calls, as well as killing us. Unnecessary in
    // SunOS: we just issue a stop here and wait for the parent thread to run
    // us again with tracing enabled.
    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    kill(getpid(), SIGSTOP);
#endif //__linux__
#if defined(__sun)
    __sol_proc_force_stop_nowait(getpid());
#endif //__sun
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

  // Restore signal handling.
  pthread_sigmask(SIG_SETMASK, &oldset, NULL);

  if(expPid < 0){
    perror("SCAF fork");
    exit(1);
  }

  int status;
#if defined(__linux__)
  if (waitpid(expPid, &status, 0) < 0) {
    perror("SCAF waitpid");
    abort();
  }

  assert(WIFSTOPPED(status));
  assert(WSTOPSIG(status) == SIGSTOP);
#endif //__linux__

#if defined(__sun)
  __sol_proc_stop_wait(expPid);
  __sol_proc_trace_sigs(expPid);
  __sol_proc_trace_syscalls(expPid);
#endif

  // Meet the other thread at the barrier. It won't start running the proper
  // instance of the section until we hit this barrier. At this point in the
  // experiment process, the instrumentation has been set up and we are just
  // about to enter the work function.
  pthread_barrier_wait(&(scaf_experiment_desc.control_pthread_b));

  int foundRaW = 0;
  int foundW = 0;
  while(1){
#if defined(__linux__)
    if (ptrace(PTRACE_SYSCALL, expPid, NULL, NULL) < 0) {
      perror("SCAF ptrace(PTRACE_SYSCALL, ...)");
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      abort();
    }
#endif //__linux__
#if defined(__sun)
    if(foundW)
       __sol_proc_run_clearsyscalls(expPid);
    else
       __sol_proc_run(expPid);

    __sol_proc_stop_wait(expPid);
    int syscall = -1;
    int signal = -1;
    lwpstatus_t ls = __sol_get_proc_status(expPid)->pr_lwp;
    if(ls.pr_why == PR_SIGNALLED){
        signal = ls.pr_what;
    }else if(ls.pr_why == PR_SYSENTRY){
        syscall = ls.pr_what;
    }
    assert(signal >= 0 || syscall >= 0);
    assert(!(signal >= 0 && syscall >= 0));
#endif //__sun

#if defined(__linux__)
    if (waitpid(expPid, &status, 0) < 0) {
      perror("SCAF waitpid");
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      abort();
    }
    int signal = WIFSTOPPED(status)? WSTOPSIG(status) : -1;

    if(!WIFSTOPPED(status) ||
        !WSTOPSIG(status)==SIGTRAP){
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      break;
    }
#endif //__linux__

#if defined(__linux__)
    if(WSTOPSIG(status)==SIGALRM){
#endif //__linux__
#if defined(__sun)
    if(signal>=0 && signal==SIGALRM){
#endif //__sun

      // The experiment has run long enough. We will stop it, but first let the
      // function run for another small period of time. This is just an easy
      // way to ensure that our experiment measurements have a minimum allowed
      // runtime.
#if defined(__linux__)
      ptrace(PTRACE_CONT, expPid, NULL, 0);
#endif //__linux__
#if defined(__sun)
      __sol_proc_run_clearsigs(expPid);
#endif //__sun
      usleep(100000);
#if defined(__linux__)
      kill(expPid, SIGALRM);
      waitpid(expPid, &status, 0);
#endif //__linux__
#if defined(__sun)
      __sol_proc_setsig(expPid, SIGALRM);
      __sol_proc_stop_wait(expPid);
#endif //__sun
#if defined(__linux__)
      ptrace(PTRACE_CONT, expPid, NULL, SIGALRM);
#endif //__linux__
#if defined(__sun)
      __sol_proc_notrace(expPid);
      __sol_proc_run(expPid);
#endif //__sun
      break;
    }

#if defined(__linux__)
    int syscall = ptrace(PTRACE_PEEKUSER, expPid, ORIG_ACCUM, 0);
    int foundUnsafeOpen = 0;

    if(syscall < 0 && signal == SIGSEGV){
      always_print(BOLDRED "WARNING: experiment %d hit SIGSEGV." RESET "\n", expPid);
      // The experiment has segfaulted, so we'll have to just stop here.
      // Deliver a SIGINT, continue the experiment, and detach. The experiment
      // process will return from the bogus/noop syscall and go straight into
      // the SIGINT signal handler.
      ptrace(PTRACE_DETACH, expPid, NULL, SIGINT);
      break;
    }
    assert(syscall >= 0);
#endif //__linux__

    if(syscall == __NR_scaf_experiment_done){
#if defined(__linux__)
      ptrace(PTRACE_DETACH, expPid, NULL, NULL);
#endif //__linux__
#if defined(__sun)
      __sol_proc_notrace(expPid);
      __sol_proc_run_clearsyscalls(expPid);
#endif
      break;
    }

#if defined(__linux__)
    if(syscall == __NR_write){
      // Replace the fd with one pointing to /dev/null. We'll keep track of any
      // following reads to prevent violating RaW hazards through the
      // filesystem. (If necessary, we could do this more precisely by tracking
      // RaWs per fd.)
      ptrace(PTRACE_POKEUSER, expPid, ARGREG, scaf_nullfd);
#endif //__linux__
#if defined(__sun)
    if(syscall == SYS_write){
#endif //__sun
      foundW = 1;
    }

#if defined(__linux__)
    if(syscall == __NR_read && foundW)
#endif //__linux__
#if defined(__sun)
    if(syscall == SYS_read && foundW)
#endif //__sun
      foundRaW = 1;

#if defined(__linux__)
    //Some opens/mmaps are safe, depending on the arguments.
    if(syscall == __NR_open){
      char *file = (char*)ptrace(PTRACE_PEEKUSER, expPid, ARGREG, 0);
      if(strcmp("/sys/devices/system/cpu/online", file)==0){
         //This is ok because it's always a read-only file.
      }else if(strcmp("/proc/stat", file)==0){
         //This is ok because it's always a read-only file.
      }else if(strcmp("/proc/meminfo", file)==0){
         //This is ok because it's always a read-only file.
      }else{
         debug_print(RED "Unsafe open: %s" RESET "\n", file);
         foundUnsafeOpen = 1;
      }
    }else if(syscall == __NR_mmap){
      int prot = (int)ptrace(PTRACE_PEEKUSER, expPid, ARG3REG, 0);
      if(prot & MAP_PRIVATE){
         //This is ok because changes won't go back to disk.
      }else if(prot & MAP_ANONYMOUS){
         //This is ok because there is no associated file.
      }else{
         foundUnsafeOpen = 1;
      }
    }
#endif //__linux__

#if defined(__linux__)
    if((syscall != __NR_rt_sigprocmask && syscall != __NR_rt_sigaction &&
          syscall != __NR_read && syscall != __NR_nanosleep &&
          syscall != __NR_write && syscall != __NR_restart_syscall &&
          syscall != __NR_mprotect && syscall != __NR_sched_getaffinity &&
          syscall != __NR_sched_setaffinity && syscall != __NR_open &&
          syscall != __NR_close && syscall != __NR_mmap &&
          syscall != __NR_fstat && syscall != __NR_munmap
        ) || foundRaW || foundUnsafeOpen){
#endif //__linux__
#if defined(__sun)
    if(( syscall != SYS_read && syscall != SYS_write )
          || foundRaW){
#endif //__sun
      // This is not one of the syscalls deemed ``safe''. (Its completion by
      // the kernel may affect the correctness of the program.) We must stop
      // the experiment fork now.
      debug_print(RED "Parent: child (%d) has behaved badly (section %p, syscall %d). Stopping it. (parent=%d)" RESET "\n", expPid, current_section_id, syscall, getpid());
#if defined(__linux__)
      void *badCall = (void*)0xbadCa11;
      if (ptrace(PTRACE_POKEUSER, expPid, ORIG_ACCUM, badCall) < 0) {
        perror("SCAF ptrace(PTRACE_POKEUSER, ...)");
        ptrace(PTRACE_KILL, expPid, NULL, NULL);
        abort();
      }
      // Deliver a SIGINT, continue the experiment, and detach. The experiment
      // process will return from the bogus/noop syscall and go straight into
      // the SIGINT signal handler.
      ptrace(PTRACE_DETACH, expPid, NULL, SIGINT);
#endif //__linux__
#if defined(__sun)
      __sol_proc_setsig(expPid, SIGINT);
      __sol_proc_notrace(expPid);
      __sol_proc_run_clearsyscalls(expPid);
#endif //__sun
      break;
    }

  } // while(1)

  // We will always have killed the child by now.
  waitpid(expPid, &status, 0);
  return NULL;
}

