#ifndef SCAF_H
#define SCAF_H

#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

// Set default hash function to  Paul Hsieh's. Seems to do lookups slightly
// faster on our keys.
#define HASH_FUNCTION HASH_SFH

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <uthash.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b; })

#if defined(__linux__)
#define __NR_scaf_experiment_done 1337
#endif //__linux__

#if defined(__sun)
// This is SYS_ntp_gettime on Solaris 10. We're stealing it. Basically SCAF
// doesn't support codes which do NTP stuff. (We can't use an unused number as
// in Linux since Solaris will kill a program making an invalid indirect
// syscall before even giving it back to the tracing process.
#define __NR_scaf_experiment_done 248
#endif //__sun

#define SCAF_MAX_CLIENT_NAME_LEN 9

static inline double pclock(){
   struct rusage ru;
   getrusage(RUSAGE_SELF, &ru);
   return (ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1.0e-6);
}

static inline double rtclock(){
   struct timeval Tp;
   gettimeofday (&Tp, NULL);
   return (Tp.tv_sec + Tp.tv_usec * 1.0e-6);
}

enum scaf_message_purpose {
   SCAF_NEW_CLIENT,
   SCAF_FORMER_CLIENT,
   SCAF_SECTION_START,
   SCAF_NOT_MALLEABLE,
   SCAF_DAEMON_FEEDBACK,
   SCAF_EXPT_START,
   SCAF_EXPT_STOP,
};

typedef struct {
   void* section_id;
   float last_time;
   float last_ipc;
   int experiment_complete;
   int first_touch_complete;
   int experiment_threads;
   float experiment_serial_ipc;
   float experiment_parallel_ipc;
   float experiment_ipc_speedup;
   float experiment_ipc_eff;
   UT_hash_handle hh;
} scaf_client_section;

// The float alignment is important for the Xeon Phi 5110p. If you're using
// some compiler that doesn't support it it will probably be ok to remove it.
typedef struct {
   void* section;
   union message_value_t {
      float efficiency __attribute__((aligned(16)));
      int experiment_pid;
   } message_value;
   int pid;
   enum scaf_message_purpose message;
} scaf_client_message;

typedef struct {
   enum scaf_message_purpose message;
   int threads;
   int num_clients;
} scaf_daemon_message;

typedef struct {
   void (*fn) (void *);
   void *data;
   pthread_t control_pthread;
   pthread_barrier_t control_pthread_b;
   pid_t experiment_pid;
} scaf_client_experiment_description;

void scaf_retire();

int scaf_section_start(void* section);

void scaf_section_end(void);

void scaf_not_malleable(void);

int scaf_gomp_experiment_create(void (*fn) (void*), void *data);
void scaf_gomp_experiment_destroy(void);

// These are just aliases for the above two functions.
int scaf_gomp_training_create(void (*fn) (void*), void *data);
void scaf_gomp_training_destroy(void);

static inline int scaf_get_num_cpus(void){
   return sysconf(_SC_NPROCESSORS_ONLN);
}

struct proc_stat {
   int pid;         // %d
   char comm[256];    // %s
   char state;       // %c
   int ppid;        // %d
   int pgrp;       // %d
   int session;      // %d
   int tty_nr;         // %d
   int tpgid;         // %d
   unsigned long flags; // %lu
   unsigned long minflt;  // %lu
   unsigned long cminflt;   // %lu
   unsigned long majflt;   // %lu
   unsigned long cmajflt; // %lu
   unsigned long utime;  // %lu
   unsigned long stime;    // %lu
   long cutime;     // %ld
   long cstime;    // %ld
   long priority;    // %ld
   long nice;       // %ld
   long num_threads;     // %ld
   long itrealvalue;    // %ld
   unsigned long starttime;  // %lu
   unsigned long vsize;  // %lu
   long rss;         // %ld
   unsigned long rlim;    // %lu
   unsigned long startcode; // %lu
   unsigned long endcode;  // %lu
   unsigned long startstack; // %lu
   unsigned long kstkesp;   // %lu
   unsigned long kstkeip;  // %lu
   unsigned long signal;  // %lu
   unsigned long blocked;   // %lu
   unsigned long sigignore;   // %lu
   unsigned long sigcatch;   // %lu
   unsigned long wchan;  // %lu
   unsigned long nswap; // %lu
   unsigned long cnswap;  // %lu
   int exit_signal;      // %d
   int processor;    // %d
   unsigned long rt_priority;   // %lu
   unsigned long policy; // %lu
   unsigned long long delayacct_blkio_ticks; // %llu
};

#ifdef SCAF_DISABLE_COLOR
#define RESET   ""
#define BLACK   ""      /* Black */
#define RED     ""      /* Red */
#define GREEN   ""      /* Green */
#define YELLOW  ""      /* Yellow */
#define BLUE    ""      /* Blue */
#define MAGENTA ""      /* Magenta */
#define CYAN    ""      /* Cyan */
#define WHITE   ""      /* White */
#define BOLDBLACK   ""      /* Bold Black */
#define BOLDRED     ""      /* Bold Red */
#define BOLDGREEN   ""      /* Bold Green */
#define BOLDYELLOW  ""      /* Bold Yellow */
#define BOLDBLUE    ""      /* Bold Blue */
#define BOLDMAGENTA ""      /* Bold Magenta */
#define BOLDCYAN    ""      /* Bold Cyan */
#define BOLDWHITE   ""      /* Bold White */
#else
#define RESET   "\033[0m"
#define BLACK   "\033[30m"      /* Black */
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */
#define BLUE    "\033[34m"      /* Blue */
#define MAGENTA "\033[35m"      /* Magenta */
#define CYAN    "\033[36m"      /* Cyan */
#define WHITE   "\033[37m"      /* White */
#define BOLDBLACK   "\033[1m\033[30m"      /* Bold Black */
#define BOLDRED     "\033[1m\033[31m"      /* Bold Red */
#define BOLDGREEN   "\033[1m\033[32m"      /* Bold Green */
#define BOLDYELLOW  "\033[1m\033[33m"      /* Bold Yellow */
#define BOLDBLUE    "\033[1m\033[34m"      /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m"      /* Bold Magenta */
#define BOLDCYAN    "\033[1m\033[36m"      /* Bold Cyan */
#define BOLDWHITE   "\033[1m\033[37m"      /* Bold White */
#endif

#if defined(DEBUG) || defined(SCAF_DEBUG)
#define DEBUG_TEST 1
#else
#define DEBUG_TEST 0
#endif

#define debug_print(...) \
   do { if (DEBUG_TEST) fprintf(stderr, __VA_ARGS__); } while (0)

#define always_print(...) \
   do { if (1) fprintf(stderr, __VA_ARGS__); } while (0)

#endif //defined SCAF_H
