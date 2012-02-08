//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2012
//
#include <zmq.h>
#include "scaf.h"

int scaf_connect(void *scafd){
   zmq_connect(scafd, SCAF_CONNECT_STRING);
   // send new client request and get initial num threads
   zmq_msg_t request;
   zmq_msg_init_size(&request, sizeof(int));
   *((int*)zmq_msg_data(&request)) = SCAF_NEW_CLIENT;
   zmq_send(scafd, &request, 0);
   zmq_msg_close(&request);

   zmq_msg_t reply;
   zmq_msg_init(&reply);
   zmq_recv(scafd, &reply, 0);
   int response = *((int*)(zmq_msg_data(&reply)));
   zmq_msg_close(&reply);
   return response;
}

void* scaf_init(void **context_p){
   void *context = zmq_init(1);
   *context_p = context;
   void *requester = zmq_socket (context, ZMQ_REQ);
   return requester;
}

int scaf_update(void *scafd){
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

