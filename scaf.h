#define SCAF_CONNECT_STRING "ipc:///tmp/ipc-scafd"
//#define SCAF_CONNECT_STRING "tcp://localhost:5555"

#define SCAF_NEW_CLIENT (1)
#define SCAF_CURRENT_CLIENT (0)
#define SCAF_FORMER_CLIENT (-1)

void* scaf_init(void **context_p);

int scaf_connect(void *scafd);

int scaf_update(void *scafd);

void scaf_retire(void *scafd, void *context);

