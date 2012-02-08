//
//  Hello World server
//  Binds REP socket to tcp://*:5555
//  Expects "Hello" from client, replies with "World"
//
#include <zmq.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "scaf.h"

int get_per_client_threads(int max_threads, int num_clients){
   if(num_clients >= max_threads)
      return 1;

   if(num_clients==0)
      return max_threads;

   return max_threads / num_clients;
}

int get_total_clients(int num_clients, int client_request){
   if(client_request == SCAF_NEW_CLIENT)
      return num_clients+1;
   else if(client_request == SCAF_CURRENT_CLIENT)
      return num_clients;
   else if(client_request == SCAF_FORMER_CLIENT){
      if(num_clients >= 1)
         return num_clients-1;
      else{
         printf("scafd: \tTOO FEW CLIENTS??\n");
         exit(1);
      }
      return num_clients;
   }
   else
      printf("scafd: \tINVALID CLIENT REQUEST!\n");

   return num_clients;
}

int main (void)
{
    void *context = zmq_init (1);
    int max_threads = 8;
    int num_clients = 0;

    //  Socket to talk to clients
    void *responder = zmq_socket (context, ZMQ_REP);
    zmq_bind (responder, SCAF_CONNECT_STRING);
    printf("scafd: Running and managing %d threads.\n", max_threads);

    while (1) {
        printf("scafd: \t%d clients.\n", num_clients);
        //  Wait for next request from client
        zmq_msg_t request;
        zmq_msg_init (&request);
        zmq_recv (responder, &request, 0);
        int client_request = *((int*)(zmq_msg_data(&request)));
        zmq_msg_close (&request);

        // Update client bookkeeping if necessary
        num_clients = get_total_clients(num_clients, client_request);
        printf("scafd: \tRequest received: %d clients.\n", num_clients);

        //  Dumbly compute the number of threads that the client should use
        int threads = get_per_client_threads(max_threads, num_clients);

        if(client_request != SCAF_FORMER_CLIENT)
           printf("scafd: \tAdvising client to use %d threads.\n", threads);

        //  Send reply back to client (even if the client has retired -- they won't hear this.
        zmq_msg_t reply;
        zmq_msg_init_size (&reply, sizeof(int));
        *((int*)(zmq_msg_data(&reply))) = threads;
        zmq_send (responder, &reply, 0);
        zmq_msg_close (&reply);
    }

    //  We never get here but if we did, this would be how we end
    zmq_close (responder);
    zmq_term (context);
    return 0;
}
