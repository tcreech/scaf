#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>

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

