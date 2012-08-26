//
//  SCAF Client library implementation
//
//  Tim Creech <tcreech@umd.edu> - University of Maryland, 2012
//
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>
#include <zmq.h>
#include <omp.h>
#include "scaf.h"
#include <papi.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/reg.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/ptrace.h>
#include <assert.h>

#if defined(__i386__)
#define ORIG_ACCUM	(4 * ORIG_EAX)
#define ARGREG	(4 * EBX)
#elif defined(__x86_64__)
#define ORIG_ACCUM	(8 * ORIG_RAX)
#define ARGREG	(8 * RDI)
#else
#error unsupported architecture
#endif

#define SCAFD_TIMEOUT_SECONDS 1

int did_scaf_startup;
void *scafd;
void *scafd_context;

int scafd_available;
int scaf_mypid;
int omp_max_threads;
int scaf_nullfd;

float scaf_section_duration;
float scaf_section_ipc;
float scaf_section_start_time;

void* current_section_id;
int* current_threads;
scaf_client_section *current_section = NULL;
scaf_client_section *sections = NULL;

void* scaf_init(void **context_p);
int scaf_connect(void *scafd);
scaf_client_section*  scaf_add_client_section(void *section_id);
scaf_client_section* scaf_find_client_section(void *section_id);

scaf_client_section inline *scaf_add_client_section(void *section_id){
   scaf_client_section *new_section = malloc(sizeof(scaf_client_section));
   new_section->section_id = section_id;
   new_section->last_time = 0;
   new_section->last_ipc = 1;
   HASH_ADD_PTR(sections, section_id, new_section);
   return new_section;
}

scaf_client_section inline *scaf_find_client_section(void *section_id){
   scaf_client_section *found = NULL;
   HASH_FIND_PTR(sections, &section_id, found);
   return found;
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
      printf("WARNING: This SCAF client could not communicate with scafd.\n");
      return omp_max_threads;
   }
}

void* scaf_init(void **context_p){
   scaf_mypid = getpid();
   scaf_nullfd = open("/dev/null", O_WRONLY | O_NONBLOCK);
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
   scaf_message->time = current_section->last_time;
   scaf_message->ipc  = current_section->last_ipc;
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

   {
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_ipc(&rtime, &ptime, &ins, &ipc);
      scaf_section_start_time = rtime;
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
   }

   current_threads = response;
   return response;
}

void scaf_section_end(void){

   if(!scafd_available)
      return;

   {
      float rtime, ptime, ipc;
      long long int ins;
      int ret = PAPI_ipc(&rtime, &ptime, &ins, &ipc);
      if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
      scaf_section_ipc = ipc;
      scaf_section_duration = (rtime - scaf_section_start_time);
   }

   current_section->last_time = scaf_section_duration;
   current_section->last_ipc  = scaf_section_ipc;

   //printf("Finished section %p. Did %f@%d, at %f IPC.\n", current_section_id, scaf_section_duration, current_threads, scaf_section_ipc);

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

void scaf_experiment_start(void){
   // Install the end of the experiment as the SIGINT handler.
   signal(SIGINT, scaf_experiment_end);
   // Also install the end of the experiment as the SIGALRM handler.
   signal(SIGALRM, scaf_experiment_end);

   // Begin gathering information with PAPI.
   float rtime, ptime, ipc;
   long long int ins;
   int ret = PAPI_ipc(&rtime, &ptime, &ins, &ipc);
   scaf_section_start_time = rtime;
   if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);

   printf("SCAF experiment started.\n");
   alarm(1);
}

void scaf_experiment_end(int sig){
   // Ignore all the signals which we might still get.
   signal(SIGALRM, SIG_IGN);
   signal(SIGINT, SIG_IGN);

   printf("SCAF experiment ending.");
   if(sig == SIGALRM){
     printf(" (took too long.)\n");
   }
   else if(sig == 0){
     printf(" (finished.)\n");
   }
   else if(sig == SIGINT){
     printf(" (killed.)\n");
   }
   else {
     printf(" (not sure why?)\n");
   }
   syscall(__NR_scaf_experiment_done);

   // Get the results from PAPI.
   float rtime, ptime, ipc;
   long long int ins;
   int ret = PAPI_ipc(&rtime, &ptime, &ins, &ipc);
   if(ret != PAPI_OK) printf("WARNING: Bad PAPI things happening. (%d)\n", ret);
   scaf_section_ipc = ipc;
   scaf_section_duration = (rtime - scaf_section_start_time);

   printf("SCAF experiment finished in %f seconds, ipc of %f.\n", scaf_section_duration, scaf_section_ipc);
   exit(0);
}

void scaf_gomp_experiment_create(void (*fn) (void *), void *data){

  int expPid = fork();
  if(expPid==0){
    // Start up our timing stuff with SCAF.
    scaf_experiment_start();
    // Request that we be traced by the parent. The parent will be in charge of
    // allowing/disallowing system calls, as well as killing us.
    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    kill(getpid(), SIGSTOP);
    // Run the parallel section in serial. The parent will intercept all
    // syscalls and ensure that we don't affect the state of the machine
    // incorrectly. An alarm has been set to keep this from taking arbitrarily
    // long.
    fn(data);
    // When finished, send ourselves SIGINT to end the experiment.
    printf("SCAF experiment sending self SIGKILL.\n");
    scaf_experiment_end(0);
    // If we get this far, just quit.
    exit(0);
  }

  if(expPid < 0){
    perror("SCAF fork");
    exit(1);
  }

  int status;
  if (waitpid(expPid, &status, 0) < 0) {
    perror("SCAF waitpid");
    abort();
  }

  assert(WIFSTOPPED(status));
  assert(WSTOPSIG(status) == SIGSTOP);

  int foundRaW = 0;
  int foundW = 0;
  while(1){
    if (ptrace(PTRACE_SYSCALL, expPid, NULL, NULL) < 0) {
      perror("SCAF ptrace(PTRACE_SYSCALL, ...)");
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      abort();
    }

    if (waitpid(expPid, &status, 0) < 0) {
      perror("SCAF waitpid");
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      abort();
    }

    if(!WIFSTOPPED(status) ||
        !WSTOPSIG(status)==SIGTRAP){
      ptrace(PTRACE_KILL, expPid, NULL, NULL);
      break;
    }

    if(WSTOPSIG(status)==SIGALRM){
      // The experiment has run long enough.
      //printf("Parent: child has taken too long. Stopping it.\n");
      ptrace(PTRACE_CONT, expPid, NULL, SIGALRM);
      break;
    }

    int syscall = ptrace(PTRACE_PEEKUSER, expPid, ORIG_ACCUM, 0);
    //printf("Parent: child has called syscall nr %d\n", syscall);

    if(syscall == __NR_scaf_experiment_done){
      //printf("Parent: this is a bogus syscall that indicates the experiment ender is in control. We'll stop tracing.\n");
      ptrace(PTRACE_DETACH, expPid, NULL, NULL);
      break;
    }

    if(syscall == __NR_write){
      // Replace the fd with one pointing to /dev/null. We'll keep track of any
      // following reads to prevent violating RaW hazards through the
      // filesystem. (If necessary, we could do this more precisely by tracking
      // RaWs per fd.)
      ptrace(PTRACE_POKEUSER, expPid, ARGREG, scaf_nullfd);
      foundW = 1;
    }
    if(syscall == __NR_read && foundW)
      foundRaW = 1;

    if((syscall != __NR_rt_sigprocmask && syscall != __NR_rt_sigaction &&
          syscall != __NR_read && syscall != __NR_nanosleep &&
          syscall != __NR_write && syscall != __NR_restart_syscall) || foundRaW){
      // This is not one of the syscalls deemed ``safe''. (Its completion by
      // the kernel may affect the correctness of the program.) We must stop
      // the experimental fork now.
      void *badCall = (void*)0xbadCa11;
      if (ptrace(PTRACE_POKEUSER, expPid, ORIG_ACCUM, badCall) < 0) {
        perror("SCAF ptrace(PTRACE_POKEUSER, ...)");
        ptrace(PTRACE_KILL, expPid, NULL, NULL);
        abort();
      }
      //printf("Parent: child has behaved badly. Stopping it.\n");
      ptrace(PTRACE_CONT, expPid, NULL, SIGINT);
      break;
    }
  }

  // We will always have killed the child by now.

  waitpid(expPid, &status, 0);
}

