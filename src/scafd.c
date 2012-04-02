//
//  SCAFd server.
//  Tim Creech <tcreech@umd.edu>, University of Maryland, 2012
//
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <zmq.h>
#include <curses.h>
#include "scaf.h"
#include "uthash.h"

#ifndef __clang__
#include <omp.h>
#else
int omp_get_max_threads();
#endif

#define MAX_CLIENTS 8

#define CURSES_INTERFACE 1

#define RD_LOCK_CLIENTS pthread_rwlock_rdlock(&clients_lock)
#define RW_LOCK_CLIENTS pthread_rwlock_wrlock(&clients_lock)
#define UNLOCK_CLIENTS pthread_rwlock_unlock(&clients_lock)

static int quiet = 0;

static scaf_client *clients = NULL;

static int max_threads;

static pthread_rwlock_t clients_lock;

scaf_client inline *find_client(int client_pid){
   scaf_client* c;
   HASH_FIND_INT(clients, &client_pid, c);
   return c;
}

void inline add_client(int client_pid, int threads){
   scaf_client *c = (scaf_client*)malloc(sizeof(scaf_client));
   c->pid = client_pid;
   c->threads = threads;
   HASH_ADD_INT(clients, pid, c);
}

void inline delete_client(scaf_client *c){
   HASH_DEL(clients, c);
}

void inline print_clients(void){
   clear();
   int i;
   int max = HASH_COUNT(clients);
   printw("scafd: Running, managing %d hardware contexts.\n\n", max_threads);
   if(max > 0){
      printw("scafd: Client list:\n");
      scaf_client *current, *tmp;
      HASH_ITER(hh, clients, current, tmp){
         printw("scafd: \t%d : %d\n", current->pid, current->threads);
      }
   }
   refresh();
}

int inline get_per_client_threads(int num_clients){
   if(num_clients >= max_threads)
      return 1;

   if(num_clients==0)
      return max_threads;

   return max_threads / num_clients;
}

int inline get_extra_threads(int num_clients){
   if(num_clients >= max_threads)
      return 0;

   if(num_clients==0)
      return 0;

   return max_threads % num_clients;
}

void inline perform_client_request(int client_request, int client_pid){
   if(client_request == SCAF_NEW_CLIENT){
      RW_LOCK_CLIENTS;
      int num_clients = HASH_COUNT(clients);
      add_client(client_pid, get_per_client_threads(num_clients+1));
      UNLOCK_CLIENTS;
      return;
   }
   else if(client_request == SCAF_CURRENT_CLIENT)
      return;
   else if(client_request == SCAF_FORMER_CLIENT){
      RW_LOCK_CLIENTS;
      scaf_client *old_client = find_client(client_pid);
      delete_client(old_client);
      UNLOCK_CLIENTS;
      free(old_client);

      return;
   }

   // Invalid request?

   return;
}

void referee_body(void* data){
#if CURSES_INTERFACE
   WINDOW *wnd;
   wnd = initscr();
   noecho();
   clear();
   refresh();
#endif
   while(1){
      RW_LOCK_CLIENTS;
      scaf_client *current, *tmp;
      int num_clients = HASH_COUNT(clients);
      int per_client  = get_per_client_threads(num_clients);
      int extra       = get_extra_threads(num_clients);
      int i=0;
      HASH_ITER(hh, clients, current, tmp){
         current->threads = per_client + ( i < extra ? 1 : 0 );
         i++;
      }
      UNLOCK_CLIENTS;

#if CURSES_INTERFACE
      RD_LOCK_CLIENTS;
      print_clients();
      UNLOCK_CLIENTS;
#endif

      sleep(1);
   }
}

void reaper_body(void* data){
   while(1){

      RD_LOCK_CLIENTS;
      scaf_client *current, *tmp;
      HASH_ITER(hh, clients, current, tmp){
         // Check if a process is dead.
         if(kill(current->pid, 0)){
            // If we actually found a dead process, then escalate and get a
            // write lock. This is assumed to happen infrequently!
            UNLOCK_CLIENTS;
            RW_LOCK_CLIENTS;
            delete_client(current);
            UNLOCK_CLIENTS;

            //When done, resume the read lock and free the allocation.
            free(current);
            RD_LOCK_CLIENTS;
         }
         //Sleep again for a bit.
         usleep(50000);
      }
      UNLOCK_CLIENTS;

      sleep(5);
   }
}

int main(int argc, char **argv){
    pthread_t referee, reaper;
    pthread_rwlock_init(&clients_lock, NULL);
    pthread_create(&referee, NULL, (void *(*)(void*))&referee_body, NULL);
    pthread_create(&reaper, NULL, (void *(*)(void*))&reaper_body, NULL);

    int c;
    while( (c = getopt(argc, argv, "q")) != -1){
       switch(c){
          case 'q':
             quiet = 1;
             break;
          default:
             // Unkonwn option ignored.
             break;
       }
    }

    void *context = zmq_init (1);
    max_threads = omp_get_max_threads();
    int num_clients = 0;

    //  Socket to talk to clients
    void *responder = zmq_socket (context, ZMQ_REP);
    zmq_bind (responder, SCAF_CONNECT_STRING);

    while (1) {
        //  Wait for next request from client
        zmq_msg_t request;
        zmq_msg_init (&request);
#if ZMQ_VERSION_MAJOR > 2
        zmq_recvmsg (responder, &request, 0);
#else
        zmq_recv (responder, &request, 0);
#endif
        scaf_client_message *client_message = (scaf_client_message*)(zmq_msg_data(&request));
        int client_pid = client_message->pid;
        int client_request = client_message->message;
        zmq_msg_close (&request);

        // Update client bookkeeping if necessary
        perform_client_request(client_request, client_pid);

        int threads=0;
        // If the client is not unsubscribing, get an answer for it.
        if(client_request != SCAF_FORMER_CLIENT){
           RD_LOCK_CLIENTS;
           threads = find_client(client_pid)->threads;
           UNLOCK_CLIENTS;
        }

        //  Send reply back to client (even if the client has retired -- they won't hear this.)
        zmq_msg_t reply;
        zmq_msg_init_size (&reply, sizeof(int));
        *((int*)(zmq_msg_data(&reply))) = threads;
#if ZMQ_VERSION_MAJOR > 2
        zmq_sendmsg (responder, &reply, 0);
#else
        zmq_send (responder, &reply, 0);
#endif
        zmq_msg_close (&reply);
    }

    //  We never get here but if we did, this would be how we end
    zmq_close (responder);
    zmq_term (context);
    return 0;
}
