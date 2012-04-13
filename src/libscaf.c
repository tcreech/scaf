//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2012
//
#include <time.h>
#include <zmq.h>
#include <omp.h>
#include "scaf.h"

#define SCAFD_TIMEOUT_SECONDS 1

int did_scaf_startup;
void *scafd;
void *scafd_context;

int scafd_available;
int scaf_mypid;
int omp_max_threads;

double scaf_section_duration;
double scaf_section_start_time;

void* current_section_id;
int* current_threads;
scaf_client_section *current_section = NULL;
scaf_client_section *sections = NULL;

// Internal function lifted from Polybench
static inline double rtclock(void);
void* scaf_init(void **context_p);
int scaf_connect(void *scafd);
scaf_client_section*  scaf_add_client_section(void *section_id);
scaf_client_section* scaf_find_client_section(void *section_id);

scaf_client_section inline *scaf_add_client_section(void *section_id){
   scaf_client_section *new_section = malloc(sizeof(scaf_client_section));
   new_section->section_id = section_id;
   HASH_ADD_PTR(sections, section_id, new_section);
   return new_section;
}

scaf_client_section inline *scaf_find_client_section(void *section_id){
   scaf_client_section *found = NULL;
   HASH_FIND_PTR(sections, &section_id, found);
   return found;
}

static inline double rtclock(){
   struct timeval Tp;
   gettimeofday(&Tp, NULL);
   return (Tp.tv_sec + Tp.tv_usec * 1.0e-6);
}

int scaf_connect(void *scafd){
   scafd_available = 1;
   zmq_pollitem_t pi;
   pi.socket = scafd;
   pi.events = ZMQ_POLLIN;

   zmq_connect(scafd, SCAF_CONNECT_STRING);
   // send new client request and get initial num threads
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(scaf_client_message));
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
   int rc = zmq_poll(&pi, 1, SCAFD_TIMEOUT_SECONDS*1000000);
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
      return omp_max_threads;
   }
}

void* scaf_init(void **context_p){
   scaf_mypid = getpid();
   void *context = zmq_init(1);
   *context_p = context;
   void *requester = zmq_socket (context, ZMQ_REQ);
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

   if(!scafd_available)
      return omp_max_threads;

   // Get num threads
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(scaf_client_message));
   scaf_client_message *scaf_message = (scaf_client_message*)(zmq_msg_data(&request));
   scaf_message->message = SCAF_SECTION_START;
   scaf_message->pid = scaf_mypid;
   scaf_message->section = section;
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
   scaf_section_start_time = rtclock();
   current_threads = response;
   return response;
}

void scaf_section_end(void){

   if(!scafd_available)
      return;

   scaf_section_duration = (rtclock() - scaf_section_start_time );

   //printf("Finished section %p. Did %f@%d.\n", current_section_id, scaf_section_duration, current_threads);

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

   return;
}

