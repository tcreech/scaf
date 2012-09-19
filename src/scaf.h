#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>

#define __NR_scaf_training_done 1337

#define SCAF_MAX_CLIENT_NAME_LEN 9

extern int scaf_nullfd;

enum scaf_message_purpose {
   SCAF_NEW_CLIENT,
   SCAF_FORMER_CLIENT,
   SCAF_SECTION_START,
   SCAF_SECTION_END
};

typedef struct {
   int pid;
   int threads;
   void* current_section;
   float efficiency;
   char name[SCAF_MAX_CLIENT_NAME_LEN+1];
   UT_hash_handle hh;
} scaf_client;

typedef struct {
   void* section_id;
   float last_time;
   float last_ipc;
   int training_complete;
   int training_threads;
   float training_serial_ipc;
   float training_parallel_ipc;
   float training_ipc_speedup;
   float training_ipc_eff;
   UT_hash_handle hh;
} scaf_client_section;

typedef struct {
   int pid;
   enum scaf_message_purpose message;
   void* section;
   float efficiency;
} scaf_client_message;

typedef struct {
   void (*fn) (void *);
   void *data;
   pthread_t control_pthread;
   pthread_barrier_t control_pthread_b;
   pid_t training_pid;
} scaf_client_training_description;

void scaf_retire();

int scaf_section_start(void* section);

void scaf_section_end(void);

int scaf_gomp_training_create(void (*fn) (void*), void *data);
void scaf_gomp_training_destroy(void);

inline void scaf_training_start(void);

inline void scaf_training_end(int);

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

