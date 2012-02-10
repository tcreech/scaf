//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2012
//
#include <zmq.h>
#include <omp.h>
#include "scaf.h"

#define SCAFD_TIMEOUT_SECONDS 1

int scafd_available;
int omp_max_threads;

int scaf_connect(void *scafd){
   scafd_available = 1;
   zmq_pollitem_t pi;
   pi.socket = scafd;
   pi.events = ZMQ_POLLIN;

   zmq_connect(scafd, SCAF_CONNECT_STRING);
   // send new client request and get initial num threads
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(int));
   *((int*)zmq_msg_data(&request)) = SCAF_NEW_CLIENT;
   zmq_send(scafd, &request, 0);
   zmq_msg_close(&request);

   // Stop and poll just to see if we timeout. If no reply, then assume there
   // is no scafd for the rest of execution.
   int rc = zmq_poll(&pi, 1, SCAFD_TIMEOUT_SECONDS*1000000);
   if(rc == 1){
      zmq_msg_t reply;
      zmq_msg_init(&reply);
      zmq_recv(scafd, &reply, 0);
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
   void *context = zmq_init(1);
   *context_p = context;
   void *requester = zmq_socket (context, ZMQ_REQ);
   return requester;
}

int scaf_update(void *scafd){
   if(!scafd_available)
      return omp_max_threads;

   // Get num threads
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(int));
   *((int*)zmq_msg_data(&request)) = SCAF_CURRENT_CLIENT;
   zmq_send(scafd, &request, 0);
   zmq_msg_close(&request);

   zmq_msg_t reply;
   zmq_msg_init(&reply);
   zmq_recv(scafd, &reply, 0);
   int response = *((int*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);
   return response;
}

void scaf_retire(void *scafd, void *context){
   // send retire request
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(int));
   *((int*)zmq_msg_data(&request)) = SCAF_FORMER_CLIENT;
   zmq_send(scafd, &request, 0);
   zmq_msg_close(&request);
   zmq_close (scafd);
   zmq_term (context);
   return;
}

