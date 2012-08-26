#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>

#define __NR_scaf_experiment_done 1337

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
   float last_time;
   float last_ipc;
   UT_hash_handle hh;
} scaf_client;

typedef struct {
   void* section_id;
   float last_time;
   float last_ipc;
   UT_hash_handle hh;
} scaf_client_section;

typedef struct {
   int pid;
   enum scaf_message_purpose message;
   void* section;
   float time;
   float ipc;
} scaf_client_message;

void scaf_retire();

int scaf_section_start(void* section);

void scaf_section_end(void);

void scaf_gomp_experiment_create(void (*fn) (void *), void *data);

void scaf_experiment_start(void);

void scaf_experiment_end(int);

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

