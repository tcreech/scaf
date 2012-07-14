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
#include <math.h>
#include <zmq.h>
#include <curses.h>
#include <time.h>
#include <papi.h>
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
static float max_ipc;
static float max_ipc_time;
static long long int max_ipc_ins;

static pthread_rwlock_t clients_lock;

// Quick test to try to get an idea of what this machine's maximum IPC is.
// Unclear now much this depends on the machine having an unloaded processor.
// This is just some nonsensical stuff to try to get any compiler to make any
// processor do some spinning.
void test_max_ipc(float* res_ipc, float* res_ipc_time, long long int* res_ins){
   float rtime, ptime, ipc;
   long long int ins;
   unsigned i=0;
   srand(time(NULL));
   unsigned upper = ((rand() % 5) + 5) * 10000000;
   double bogus = 0.0;
   PAPI_ipc(&rtime, &ptime, &ins, &ipc);
   for(i=0; i<upper; ++i){
      bogus += (((i*i*i*i*i+i+i+i+i)%100)+1);
      bogus += (((i*i*i*i*i+i+i+i+i)%100)+1);
      bogus += (((i*i*i*i*i+i+i+i+i)%100)+1);
      bogus += (((i*i*i*i*i+i+i+i+i)%100)+1);
   }
   //printf("%d\n", bogus);
   PAPI_ipc(&rtime, &ptime, &ins, &ipc);
   srand((int)bogus);
   *res_ipc = ipc;
   *res_ipc_time = rtime;
   *res_ins = ins;
}

scaf_client inline *find_client(int client_pid){
   scaf_client* c;
   HASH_FIND_INT(clients, &client_pid, c);
   return c;
}

void inline add_client(int client_pid, int threads, void* client_section){
   scaf_client *c = (scaf_client*)malloc(sizeof(scaf_client));
   bzero((void*)c, sizeof(scaf_client));
   c->pid = client_pid;
   c->threads = threads;
   c->current_section = client_section;
   HASH_ADD_INT(clients, pid, c);
}

void inline delete_client(scaf_client *c){
   HASH_DEL(clients, c);
}

void inline print_clients(void){
   clear();
   int i;
   int max = HASH_COUNT(clients);
   printw("scafd: Running, managing %d hardware contexts. %d clients. Max IPC is %f, measured for %fs and %lld insts.\n\n", max_threads, max, max_ipc, max_ipc_time, max_ipc_ins);
   if(max > 0){
      //printw("PID\tTHREADS\tSECTION\tTIME/IPC\tEFFICIENCY\n");
      printw("%-06s%-08s%-09s%-10s%-10s%-10s\n", "PID", "THREADS", "SECTION", "TIME", "IPC", "EFFICIENCY");
      scaf_client *current, *tmp;
      HASH_ITER(hh, clients, current, tmp){
         printw("%-06d%-08d%-09p%-10f%-10f%-10f\n",current->pid, current->threads, current->current_section, current->last_time, current->last_ipc, current->last_ipc / max_ipc);
         //printw("%d\t%d\t%p\t%f/%f\t%f\n", current->pid, current->threads, current->current_section, current->last_time, current->last_ipc, current->last_ipc / max_ipc);
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

int inline perform_client_request(scaf_client_message *client_message){
   int client_pid = client_message->pid;
   int client_request = client_message->message;

   int client_threads;
   if(client_request == SCAF_NEW_CLIENT){
      RW_LOCK_CLIENTS;
      int num_clients = HASH_COUNT(clients);
      client_threads = get_per_client_threads(num_clients+1);
      add_client(client_pid, client_threads, client_message->section);
      UNLOCK_CLIENTS;
      return client_threads;
   }
   else if(client_request == SCAF_SECTION_START){
      RW_LOCK_CLIENTS;
      scaf_client *client = find_client(client_pid);
      client->current_section = client_message->section;
      client->last_time = client_message->time;
      client->last_ipc  = client_message->ipc;
      client_threads = client->threads;
      UNLOCK_CLIENTS;
      return client_threads;
   }
   else if(client_request == SCAF_SECTION_END){
      RW_LOCK_CLIENTS;
      scaf_client *client = find_client(client_pid);
      UNLOCK_CLIENTS;
      return 0;
   }
   else if(client_request == SCAF_FORMER_CLIENT){
      RW_LOCK_CLIENTS;
      scaf_client *old_client = find_client(client_pid);
      delete_client(old_client);
      UNLOCK_CLIENTS;
      free(old_client);

      return 0;
   }

   // Invalid request?

   return 0;
}

void referee_body(void* data){
   while(1){
      RW_LOCK_CLIENTS;
      scaf_client *current, *tmp;

      float ipc_sum = 0.0;

      int i=0;
      HASH_ITER(hh, clients, current, tmp){
         ipc_sum += current->last_ipc;
         i++;
      }
      float proc_ipc = ((float)(max_threads)) / ipc_sum;

      i=0;
      HASH_ITER(hh, clients, current, tmp){
         current->threads = (int)roundf(current->last_ipc * proc_ipc);
         i++;
      }
      UNLOCK_CLIENTS;

      usleep(250000);
   }
}

void scoreboard_body(void* data){
#if CURSES_INTERFACE
   WINDOW *wnd;
   wnd = initscr();
   noecho();
   clear();
   refresh();
#endif
   while(1){
#if CURSES_INTERFACE
      RD_LOCK_CLIENTS;
      print_clients();
      UNLOCK_CLIENTS;
#endif
      usleep(250000);
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
    test_max_ipc(&max_ipc, &max_ipc_time, &max_ipc_ins);

    pthread_t referee, reaper, scoreboard;
    pthread_rwlock_init(&clients_lock, NULL);
    pthread_create(&scoreboard, NULL, (void *(*)(void*))&scoreboard_body, NULL);
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
        // Update client bookkeeping if necessary
        int threads = perform_client_request(client_message);
        zmq_msg_close (&request);

        //  Send reply back to client (even if the client doesn't care about an answer)
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
