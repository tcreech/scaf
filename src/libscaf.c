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
#include <omp.h>
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

#if defined(__i386__)
#define ORIG_ACCUM	(4 * ORIG_EAX)
#define ARGREG	(4 * EBX)
#elif defined(__x86_64__)
#define ORIG_ACCUM	(8 * ORIG_RAX)
#define ARGREG	(8 * RDI)
#elif defined(__tilegx__)
// This is for TileGx, which is 64-bit. Guessing this stuff mostly.
#define ORIG_ACCUM   (8 * TREG_SYSCALL_NR)
#define ARGREG      (8 * 0)
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

// Training-related options
// Set this to 1 if parallel execution should wait for training to finish;
// otherwise it will be forced to terminate early.
#define SCAF_PARALLEL_WAIT_FOR_TRAINING 0
// Set to 1 if SCAF should enforce a hard limit on the amount of time spent
// running a serial experiment.
#define SCAF_ENFORCE_TRAINING_TIME_LIMIT 0
// The maximum amount of time that a training fork will run for.
#define SCAF_TRAINING_TIME_LIMIT_SECONDS 10

// Disable training
#define SCAF_ENABLE_TRAINING 1

// Limit measured efficiency to this value. This can restrict the effects of
// timing glitches resulting in crazy values.
#define SCAF_MEASURED_EFF_LIMIT 2.0

#if defined(__sun)
#define SCAF_LOWPASS_TIME_CONSTANT (15.0)
#else //__sun
#define SCAF_LOWPASS_TIME_CONSTANT (2.0)
#endif //__sun

// PAPI high-level event to measure scalability by
//#define PAPI_HL_MEASURE PAPI_flops
//#define PAPI_HL_MEASURE PAPI_flips
#define PAPI_HL_MEASURE PAPI_ipc

void* scaf_gomp_training_control(void *unused);
scaf_client_training_description scaf_training_desc;
static inline void scaf_training_start(void);
static inline void scaf_training_end(int);

int did_scaf_startup;
void *scafd;
void *scafd_context;

int scafd_available;
int scaf_disable_training = 0;
int scaf_mypid;
int omp_max_threads;
int scaf_nullfd;
int scaf_feedback_freq = 1;

double scaf_init_rtclock;
float scaf_section_duration;
float scaf_section_ipc;
float scaf_section_start_time;
float scaf_section_end_time;
float scaf_serial_duration;
float scaf_section_efficiency;
static pthread_mutex_t scaf_section_ipc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t scaf_master_thread;

void* current_section_id;
int current_threads;
scaf_client_section *current_section = NULL;
scaf_client_section *sections = NULL;

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

void* scaf_init(void **context_p);
int scaf_connect(void *scafd);
scaf_client_section*  scaf_add_client_section(void *section_id);
scaf_client_section* scaf_find_client_section(void *section_id);

scaf_client_section inline *scaf_add_client_section(void *section_id){
   scaf_client_section *new_section = malloc(sizeof(scaf_client_section));
   new_section->section_id = section_id;
   new_section->last_time = 0;
   new_section->last_ipc = 1;
   new_section->training_complete = 0;
   new_section->training_serial_ipc = 0.5;
   HASH_ADD_PTR(sections, section_id, new_section);
   return new_section;
}

scaf_client_section inline *scaf_find_client_section(void *section_id){
   scaf_client_section *found = NULL;
   HASH_FIND_PTR(sections, &section_id, found);
   return found;
}

int scaf_connect(void *scafd){
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
      int response = *((int*)(zmq_msg_data(&reply)));
      zmq_msg_close(&reply);

      return response;
   } else {
      // No response.
      scafd_available = 0;
      omp_max_threads = omp_get_max_threads();
      printf("WARNING: This SCAF client could not communicate with scafd.\n");
      return omp_max_threads;
   }
}

void* scaf_init(void **context_p){
   scaf_mypid = getpid();
   scaf_init_rtclock = rtclock();
   scaf_nullfd = open("/dev/null", O_WRONLY | O_NONBLOCK);
   char *fbf = getenv("SCAF_FEEDBACK_FREQ");
   if(fbf)
      scaf_feedback_freq = atoi(fbf);
   else
      scaf_feedback_freq = 1;

   char *notrain = getenv("SCAF_DISABLE_TRAINING");
   if(notrain)
     scaf_disable_training = atoi(notrain);
   else
     scaf_disable_training = 0;

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
   //printf("Starting section %p.\n", section);
   static int rate_limit_counter = 0;
   int skip_this_communication = 0;

   current_section_id = section;
   if(current_section == NULL || current_section->section_id != section){
      current_section = scaf_find_client_section(current_section_id);
      if(current_section == NULL){
         current_section = scaf_add_client_section(current_section_id);
      }

      if(!did_scaf_startup){
         scafd = scaf_init(&scafd_context);
         did_scaf_startup=1;

         // scaf_connect gives a reply, but we just ignore it.
         scaf_connect(scafd);
      }
   }

   if(!scafd_available){
      current_threads = omp_max_threads;
      return omp_max_threads;
   }

   if(((rate_limit_counter++) % scaf_feedback_freq) != 0){
      //current_threads = current_threads;
      skip_this_communication = 1;
   }

   if(current_threads < 1)
      current_threads=1;
   if(scaf_section_duration <= 0.000001){
      scaf_section_duration = 0.01;
      scaf_section_efficiency = 0.5;
   }

   float scaf_serial_efficiency = 1.0 / current_threads;
   float scaf_latest_efficiency_duration = (scaf_section_duration + scaf_serial_duration);
   float scaf_latest_efficiency = (scaf_section_efficiency * scaf_section_duration + scaf_serial_efficiency * scaf_serial_duration) / scaf_latest_efficiency_duration;
   float scaf_latest_efficiency_smooth = lowpass(scaf_latest_efficiency, scaf_latest_efficiency_duration, SCAF_LOWPASS_TIME_CONSTANT);
   //printf(BLUE "Latest: %f; raw: %f; stime: %f; ptime %f; seff: %f; peff: %f\n" RESET, scaf_latest_efficiency_smooth, scaf_latest_efficiency, scaf_serial_duration, scaf_section_duration, scaf_serial_efficiency, scaf_section_efficiency);

   // Communicate the latest results with the SCAF daemon and get an allocation update, but only if this wouldn't exceed our desired communication rate.
   if(!skip_this_communication){
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
      int response = *((int*)(zmq_msg_data(&reply)));
      zmq_msg_close(&reply);
      current_threads = response;
   }

#if(HAVE_LIBPAPI)
   {
      float ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&scaf_section_start_time, &ptime, &ins, &ipc);
      scaf_serial_duration = scaf_section_start_time - scaf_section_end_time;
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
   }
#else
   {
      scaf_section_start_time = (float)(rtclock() - scaf_init_rtclock);
      scaf_serial_duration = scaf_section_start_time - scaf_section_end_time;
   }
#endif

   scaf_section_ipc = 0.0;

#if(HAVE_LIBPAPI)
   if(!current_section->training_complete && scafd_available)
      return current_threads-1;
#endif

   return current_threads;
}

void scaf_section_end(void){

   if(!scafd_available)
      return;

#if(HAVE_LIBPAPI)
   {
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&rtime, &ptime, &ins, &ipc);
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
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
   scaf_section_efficiency = min(SCAF_MEASURED_EFF_LIMIT, scaf_section_ipc / current_section->training_serial_ipc);

#if 0 // We don't do anything at all with "SECTION_END" messages.
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(scaf_client_message));
   scaf_client_message *scaf_message = (scaf_client_message*)(zmq_msg_data(&request));
   scaf_message->message = SCAF_SECTION_END;
   scaf_message->pid = scaf_mypid;
   scaf_message->section = current_section_id;
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
   int response = *((int*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);
#endif

   return;
}

inline void scaf_training_start(void){

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
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
   }
#else
   {
      scaf_section_start_time = 1.0;
   }
#endif

   // Install the end of the training as the SIGINT handler.
   signal(SIGINT, scaf_training_end);
   // Also install the end of the training as the SIGALRM handler.
   signal(SIGALRM, scaf_training_end);

   //printf(BLUE "SCAF training started." RESET "\n");
   if(SCAF_ENFORCE_TRAINING_TIME_LIMIT)
      alarm(SCAF_TRAINING_TIME_LIMIT_SECONDS);
}

inline void scaf_training_end(int sig){

   syscall(__NR_scaf_training_done);

   // Ignore all the signals which we might still get.
   signal(SIGALRM, SIG_IGN);
   signal(SIGINT, SIG_IGN);

   //printf(BLUE "SCAF training ending.");
   if(sig == SIGALRM){
     //printf(" (took too long.)\n");
   }
   else if(sig == 0){
     //printf(" (finished.)\n");
   }
   else if(sig == SIGINT){
     //printf(" (killed.)\n");
   }
   else {
     //printf(" (not sure why?)\n");
   }
   //printf(RESET);

#if(HAVE_LIBPAPI)
   {
      // Get the results from PAPI.
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_HL_MEASURE(&rtime, &ptime, &ins, &ipc);
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
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

   //printf(BLUE "SCAF training (%p) finished in %f seconds, ipc of %f." RESET "\n", current_section->section_id, scaf_section_duration, scaf_section_ipc);

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
   int response = *((int*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);

   zmq_close(scafd);
   exit(0);
}

int scaf_gomp_training_create(void (*fn) (void*), void *data){
   // First of all, only train if necessary.
   if(scaf_disable_training || !scafd_available || current_section->training_complete || !(current_threads>1))
      return 0;

#if(! HAVE_LIBPAPI || ! SCAF_ENABLE_TRAINING)
   {
      return 0;
   }
#endif

   scaf_training_desc.fn = fn;
   scaf_training_desc.data = data;
   pthread_barrier_init(&(scaf_training_desc.control_pthread_b), NULL, 2);
   pthread_create(&(scaf_training_desc.control_pthread), NULL, &scaf_gomp_training_control, NULL);
   pthread_barrier_wait(&(scaf_training_desc.control_pthread_b));
   return 1;
}

void scaf_gomp_training_destroy(void){
   // First of all, only train if necessary.
   if(scaf_disable_training || !scafd_available || current_section->training_complete || !(current_threads>1))
      return;

#if(! HAVE_LIBPAPI || ! SCAF_ENABLE_TRAINING)
   {
      return;
   }
#endif

#if(! SCAF_PARALLEL_WAIT_FOR_TRAINING)
   {
      kill(scaf_training_desc.training_pid, SIGALRM);
   }
#endif

   // Get the training results from the child process.
   void *training_child = zmq_socket(scafd_context, ZMQ_REP);
   char child_connect_string[64];
   sprintf(child_connect_string, "ipc:///tmp/scaf-ipc-%d", scaf_mypid);
   assert(0==zmq_bind(training_child, child_connect_string));
   zmq_msg_t reply;
   zmq_msg_init(&reply);
#if ZMQ_VERSION_MAJOR > 2
      zmq_recvmsg(training_child, &reply, 0);
#else
      zmq_recv(training_child, &reply, 0);
#endif
   float response = *((float*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);
   //  Send reply back to client (even if the client doesn't care about an answer)
   zmq_msg_init_size (&reply, sizeof(int));
   *((int*)(zmq_msg_data(&reply))) = 0;
#if ZMQ_VERSION_MAJOR > 2
   zmq_sendmsg (training_child, &reply, 0);
#else
   zmq_send (training_child, &reply, 0);
#endif
   zmq_msg_close (&reply);
   zmq_close(training_child);

   pthread_join(scaf_training_desc.control_pthread, NULL);
   current_section->training_threads = current_threads-1;
   current_section->training_serial_ipc = response;
   current_section->training_parallel_ipc = scaf_section_ipc;
   current_section->training_ipc_eff = scaf_section_ipc / response;
   current_section->training_ipc_speedup = ((float)current_section->training_threads) * current_section->training_ipc_eff;
   current_section->training_complete = 1;
   //printf(BLUE "Section (%p): @(1,%d){%f}{sIPC: %f; pIPC: %f} -> {EFF: %f; SPU: %f}" RESET "\n", current_section->section_id, current_section->training_threads, scaf_section_duration, current_section->training_serial_ipc, current_section->training_parallel_ipc, current_section->training_ipc_eff, current_section->training_ipc_speedup);
}

void* scaf_gomp_training_control(void *unused){
   void (*fn) (void*) = scaf_training_desc.fn;
   void *data = scaf_training_desc.data;

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
  scaf_training_desc.training_pid = expPid;
  if(expPid==0){
    // Put ourselves in the parent's process group.
    setpgid(0, scaf_mypid);
    // Start up our timing stuff with SCAF.
    scaf_training_start();


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
    // When finished, send ourselves SIGINT to end the training.
    scaf_training_end(0);
    // If we get this far, just quit.
    exit(0);
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
  // training process, the instrumentation has been set up and we are just
  // about to enter the work function.
  pthread_barrier_wait(&(scaf_training_desc.control_pthread_b));

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

      // The training has run long enough. We will stop it, but first let the
      // function run for another small period of time. This is just an easy
      // way to ensure that our training measurements have a minimum allowed
      // runtime.
#if defined(__linux__)
      ptrace(PTRACE_CONT, expPid, NULL, 0);
#endif //__linux__
#if defined(__sun)
      __sol_proc_run_clearsigs(expPid);
#endif //__sun
      usleep(100000);
#if defined(__linux__)
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
#endif //__linux__

    assert(syscall >= 0);
    if(syscall == __NR_scaf_training_done){
      //printf(RED "Parent: this is a bogus syscall that indicates the training ender is in control. We'll stop tracing.\n" RESET);
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
    if(syscall == __NR_open){
      //Some opens are safe, depending on the arguments.
      char *file = ptrace(PTRACE_PEEKUSER, expPid, ARGREG, 0);
      if(strcmp("/sys/devices/system/cpu/online", file)==0){
         //This is ok because it's always a read-only file.
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
          syscall != __NR_close
        ) || foundRaW || foundUnsafeOpen){
#endif //__linux__
#if defined(__sun)
    if(( syscall != SYS_read && syscall != SYS_write )
          || foundRaW){
#endif //__sun
      // This is not one of the syscalls deemed ``safe''. (Its completion by
      // the kernel may affect the correctness of the program.) We must stop
      // the training fork now.
      printf("Parent: child has behaved badly (section %p, syscall %d). Stopping it.\n", current_section_id, syscall);
#if defined(__linux__)
      void *badCall = (void*)0xbadCa11;
      if (ptrace(PTRACE_POKEUSER, expPid, ORIG_ACCUM, badCall) < 0) {
        perror("SCAF ptrace(PTRACE_POKEUSER, ...)");
        ptrace(PTRACE_KILL, expPid, NULL, NULL);
        abort();
      }
      ptrace(PTRACE_CONT, expPid, NULL, SIGINT);
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

