#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#define SCAF_NEW_CLIENT (-1)
#define SCAF_CURRENT_CLIENT (-2)
#define SCAF_FORMER_CLIENT (-3)
#define SCAF_SECTION_START (-4)
#define SCAF_SECTION_END (-5)

#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>

typedef struct {
   int pid;
   int threads;
   double time;
   char locked;
   UT_hash_handle hh;
} scaf_client;

typedef struct {
   int pid;
   int message;
   double time;
} scaf_client_message;

void scaf_retire();

int scaf_section_start(void);

void scaf_section_end(void);

