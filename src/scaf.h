#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#define SCAF_NEW_CLIENT (-1)
#define SCAF_CURRENT_CLIENT (-2)
#define SCAF_FORMER_CLIENT (-3)

#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>

typedef struct {
   int pid;
   int threads;
   char locked;
   UT_hash_handle hh;
} scaf_client;

typedef struct {
   int pid;
   int message;
} scaf_client_message;

void* scaf_init(void **context_p);

int scaf_connect(void *scafd);

int scaf_update(void *scafd);

void scaf_retire(void *scafd, void *context);

