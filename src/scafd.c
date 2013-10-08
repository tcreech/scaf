//
//  SCAFd server.
//  Tim Creech <tcreech@umd.edu>, University of Maryland, 2012
//
#include "../config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include <pthread.h>
#include <assert.h>
#include <math.h>
#include <zmq.h>
#include <curses.h>
#include <time.h>
#include <strings.h>
#include "scaf.h"
#include "uthash.h"

#ifndef __clang__
#include <omp.h>
#else
int omp_get_max_threads();
#endif

#if defined(__sun)
#include "solaris_trace_utils.h"
#endif //__sun

#define MAX_CLIENTS 8

#define REFEREE_PERIOD_US (250000)

#define RD_LOCK_CLIENTS pthread_rwlock_rdlock(&clients_lock)
#define RW_LOCK_CLIENTS pthread_rwlock_wrlock(&clients_lock)
#define UNLOCK_CLIENTS pthread_rwlock_unlock(&clients_lock)

#define MAX(a,b) ((a > b) ? a : b)
#define MIN(a,b) ((a < b) ? a : b)

static int nobgload = 0;
static int equipartitioning = 0;
static int curses_interface = 0;
static int text_interface = 0;
static char *logfilename = NULL;

static scaf_client *clients = NULL;

static int max_threads;
static float bg_utilization;

static pthread_rwlock_t clients_lock;

static FILE *lf;
static double startuptime;

int get_scaf_controlled_pids(int** pid_list){
   RD_LOCK_CLIENTS;
   int size = HASH_COUNT(clients);
   *pid_list = malloc(sizeof(int)*size);

   scaf_client *current, *tmp;
   int i=0;
   HASH_ITER(hh, clients, current, tmp){
      (*pid_list)[i] = current->pid;
      i++;
   }
   UNLOCK_CLIENTS;
   return size;
}

int pid_is_scaf_controlled(int pid, int* pid_list, int list_size){
   int i;
   int pgid = getpgid(pid);
   for(i=0; i<list_size; i++){
      if(pid_list[i] == pid || pid_list[i] == pgid){
         return 1;
      }
   }
   return 0;
}

// Go through a /proc/ filesystem and get jiffy usage for all processes not
// under scafd's control. This takes 1 second to run due to a 1 second sleep.
// Might possibly work on FreeBSD as is if we use /compat/linux/proc/ instead
// of /proc/. Can certainly be implemented for SunOS, but this is TODO. (SunOS
// version of this acts as if everything is idle always.)
float proc_get_cpus_used(void){
#if defined(__linux__)
   // Return immediately if the user disabled load monitoring
   if(nobgload){
      sleep(1);
      return 0.0;
   }

   unsigned long g_user[2], g_low[2], g_sys[2], g_idle[2], g_iow[2], g_hirq[2], g_sirq[2];
   const char *format = "%d %s %c %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %lu %lu %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %lu %lu %llu";

   int* pid_list;
   int list_size = get_scaf_controlled_pids(&pid_list);
   unsigned long used_stime[2], used_utime[2], scaf_stime[2], scaf_utime[2];

   int z;
   for(z=0; z<2; z++){
      FILE *fp;
      fp = fopen("/proc/stat","r");
      assert(fp && "Can't open $PROC/stat ?");
      assert(7==fscanf(fp,"cpu %lu %lu %lu %lu %lu %lu %lu", &(g_user[z]), &(g_low[z]), &(g_sys[z]), &(g_idle[z]), &(g_iow[z]), &(g_hirq[z]), &(g_sirq[z])));
      fclose(fp);

      used_utime[z] = 0;
      used_stime[z] = 0;
      scaf_utime[z] = 0;
      scaf_stime[z] = 0;

      DIR* procroot;
      procroot = opendir("/proc");
      struct dirent *procdir;
      while((procdir = readdir(procroot))){
         int procdir_pid = atoi(procdir->d_name);
         if(procdir_pid == 0)
            continue;

         bool is_scaf_pid = 0;
         if(pid_is_scaf_controlled(procdir_pid, pid_list, list_size)){
            is_scaf_pid = 1;
         }

         FILE *stat_f;
         char buf[256];
         sprintf(buf, "/proc/%d/stat",procdir_pid);
         stat_f = fopen(buf, "r");
         struct proc_stat stat;
         if(stat_f){
            struct proc_stat *s = &stat;
            // Parse the /proc/$PID/stat file for jiffie usage
            int ret = fscanf(stat_f, format, &s->pid, s->comm, &s->state, &s->ppid, &s->pgrp, &s->session, &s->tty_nr, &s->tpgid, &s->flags, &s->minflt, &s->cminflt, &s->majflt, &s->cmajflt, &s->utime, &s->stime, &s->cutime, &s->cstime, &s->priority, &s->nice, &s->num_threads, &s->itrealvalue, &s->starttime, &s->vsize, &s->rss, &s->rlim, &s->startcode, &s->endcode, &s->startstack, &s->kstkesp, &s->kstkeip, &s->signal, &s->blocked, &s->sigignore, &s->sigcatch, &s->wchan, &s->nswap, &s->cnswap, &s->exit_signal, &s->processor, &s->rt_priority, &s->policy, &s->delayacct_blkio_ticks);
            fclose(stat_f);
            if(ret == 42){
               if(is_scaf_pid){
                  // If this process is scaf controlled, record its usage so
                  // that we can account for missing non-idle jiffies
                  scaf_utime[z] += stat.utime;
                  //scaf_stime[z] += stat.stime;
               } else {
                  // If this is an uncontrolled process, record its usage so
                  // that we can avoid oversubscription later
                  used_utime[z] += stat.utime;
                  //used_stime[z] += stat.stime;
               }
            } else {
               printf("WARNING: failed to parse /proc/%d/stat !\n", procdir_pid);
            }
         }
      }
      closedir(procroot);
      if(z!=1){
         sleep(1);
         list_size = get_scaf_controlled_pids(&pid_list);
      }
   }

   int long total_used = (g_user[1] - g_user[0]) + (g_low[1] - g_low[0]) + (g_sys[1] - g_sys[0]) + (g_iow[1] - g_iow[0]) + (g_hirq[1] - g_hirq[0]) + (g_sirq[1] - g_sirq[0]);
   int long scaf_used = (scaf_utime[1] - scaf_utime[0]) + (scaf_stime[1] - scaf_stime[0]);
   // Note that the scaf jiffies are reclaimed as "idle" here.
   int long total_idle = (g_idle[1] - g_idle[0]) + (scaf_used>0?scaf_used:0);

   int long non_scaf_used  = (used_utime[1] - used_utime[0]) + (used_stime[1] - used_stime[0]);
   int long used = total_used - scaf_used;
   int long total = used + total_idle;

   double utilization = (((double)used) / ((double)total));
   utilization = fmin(fmax(utilization,0.0), 1.0);
   utilization *= (double)max_threads;
   //printf("total_used: %ld; total_idle: %ld; used: %ld; total: %ld; util: %f\n", total_used, total_idle, used, total, utilization);

   free(pid_list);
   return utilization;
#endif //__linux__

#if defined(__sun)
      sleep(1);
      return 0.0;
#endif //__sun
}

scaf_client *find_client(int client_pid){
   scaf_client* c;
   HASH_FIND_INT(clients, &client_pid, c);
   return c;
}

char* gnu_basename(char *path){
   char *base = strrchr(path, '/');
   return base ? base+1 : path;
}

void get_name_from_pid(int pid, char *buf){
#if defined(__linux__)
   char procpath[64];
   char exe[1024];
   sprintf(procpath, "/proc/%d/exe", pid);
   ssize_t s = readlink(procpath, exe, 1023);
   if(s >= 0){
      exe[s] = '\0';
      char *name = gnu_basename(exe);
      strncpy(buf, name, SCAF_MAX_CLIENT_NAME_LEN);
   }else{
      char name[5] = "[??]\0";
      strncpy(buf, name, 5);
   }
#endif //__linux__
#if defined(__sun)
   psinfo_t pi = *__sol_get_proc_info(pid);
   strncpy(buf, pi.pr_fname, PRFNSZ);
#endif //__sun
}

void add_client(int client_pid, int threads, void* client_section){
   scaf_client *c = (scaf_client*)malloc(sizeof(scaf_client));
   bzero((void*)c, sizeof(scaf_client));
   c->pid = client_pid;
   c->threads = threads;
   c->current_section = client_section;
   c->metric = 1.0;
   get_name_from_pid(client_pid, c->name);
   HASH_ADD_INT(clients, pid, c);
}

void delete_client(scaf_client *c){
   HASH_DEL(clients, c);
}

void text_print_clients(void){
   printf("%-06s%-09s%-08s%-09s%-10s\n", "PID", "NAME", "THREADS", "SECTION", "EFFICIENCY");
   printf("%-06s%-09s%-08d%-09s%-10s\n", "all", "-", max_threads, "-", "-");
   scaf_client *current, *tmp;
   HASH_ITER(hh, clients, current, tmp){
      printf("%-06d%-09s%-08d%-09p%-10f\n", current->pid, current->name, current->threads, current->current_section, current->metric);
   }
   printf("\n");
}

void curses_print_clients(void){
   move(0,0); clrtobot();
   int i;
   int max = HASH_COUNT(clients);
   start_color();
   init_pair(1, COLOR_WHITE, COLOR_BLUE);
   init_pair(2, COLOR_WHITE, COLOR_RED);

   attron(COLOR_PAIR(1));
   attron(A_BOLD);
   printw("scafd: Running, managing %d hardware contexts. ", max_threads);
#if(HAVE_LIBPAPI)
   printw("Runtime experiments supported.\n");
#else
   attroff(COLOR_PAIR(1));
   attron(COLOR_PAIR(2));
   printw("WARNING: Runtime experiments NOT supported.\n");
   attroff(COLOR_PAIR(2));
   attron(COLOR_PAIR(1));
#endif
   printw("%d clients. Uncontrollable utilization: %f\n", max, bg_utilization);
   attroff(COLOR_PAIR(1));
   attroff(A_BOLD);
   hline(0, 1024); move(3,0);
   attron(A_BOLD);

   if(max > 0){
      //printw("PID\tTHREADS\tSECTION\tTIME/IPC\tEFFICIENCY\n");
      attron(COLOR_PAIR(2));
      printw("%-06s%-09s%-08s%-09s%-10s%-10s%-10s\n", "PID", "NAME", "THREADS", "SECTION", "TIME", "IPC", "EFFICIENCY");
      attroff(COLOR_PAIR(2));
      attroff(A_BOLD);
      scaf_client *current, *tmp;
      HASH_ITER(hh, clients, current, tmp){
         printw("%-06d%-09s%-08d%-09p%-10f%-10f%-10f\n", current->pid, current->name, current->threads, current->current_section, 0.0, 0.0, current->metric);
      }
   }else{
      attron(COLOR_PAIR(2));
      printw("(No SCAF processes found.)\n");
      attroff(COLOR_PAIR(2));
      attroff(A_BOLD);
   }
   refresh();
}

int get_per_client_threads(int num_clients){
   if(num_clients >= max_threads)
      return 1;

   if(num_clients==0)
      return max_threads;

   return max_threads / num_clients;
}

int get_extra_threads(int num_clients){
   if(num_clients >= max_threads)
      return 0;

   if(num_clients==0)
      return 0;

   return max_threads % num_clients;
}

int perform_client_request(scaf_client_message *client_message){
   int client_pid = client_message->pid;
   int client_request = client_message->message;

   float client_metric = equipartitioning ? 0.5 : client_message->efficiency;

   if(client_metric == 0.0)
      client_metric += 0.1;

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
      assert(client);
      client->current_section = client_message->section;
      client->metric = client_metric;
      client_threads = client->threads;
      UNLOCK_CLIENTS;
      return client_threads;
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

void maxspeedup_referee_body(void* data){
   while(1){
      RW_LOCK_CLIENTS;
      scaf_client *current, *tmp;

      float metric_sum = 0.0;
      int num_clients = HASH_COUNT(clients);

      int i=0;
      HASH_ITER(hh, clients, current, tmp){
         current->log_factor = MAX((current->metric * current->threads - 1.0),1.0)/log(current->threads);
         metric_sum += current->log_factor;
         i++;
      }

      int available_threads = max_threads - ceil(bg_utilization - 0.5);
      int remaining_rations = MAX(available_threads, 1);
      float proc_ipc = ((float)(remaining_rations)) / metric_sum;

      i=0;
      HASH_ITER(hh, clients, current, tmp){
         float exact_ration = current->log_factor * proc_ipc;
         int min_ration = floor(exact_ration);
         current->threads = min_ration==0?1:min_ration;
         remaining_rations -= current->threads;
         i++;
      }

      i=0;
      double logentrytime = rtclock() - startuptime;
      HASH_ITER(hh, clients, current, tmp){
         if(remaining_rations!=0){

            float exact_ration = current->log_factor * proc_ipc;
            if(exact_ration > current->threads){
               current->threads++;
               remaining_rations--;
            }
         }
         if(logfilename){
            fprintf(lf, "%g, %d, %g, %d\n", logentrytime, current->pid, current->metric, current->threads);
            //fflush(lf);
         }
      }
      UNLOCK_CLIENTS;

      usleep(REFEREE_PERIOD_US);
   }
}

void equi_referee_body(void* data){
   while(1){
      RW_LOCK_CLIENTS;
      scaf_client *current, *tmp;

      int num_clients = HASH_COUNT(clients);

      int i=0;

      int available_threads = max_threads - ceil(bg_utilization - 0.5);
      int remaining_rations = MAX(available_threads, 1);

      if(num_clients > 0){
         int per_client = remaining_rations / num_clients;
         int extra = remaining_rations % num_clients;

         i=0;
         HASH_ITER(hh, clients, current, tmp){
            current->threads = per_client + (i<extra?1:0);
            i++;
            if(logfilename){
               fprintf(lf, "%g, %d, %g, %d\n", rtclock()-startuptime, current->pid, 0.5, current->threads);
               //fflush(lf);
            }
         }
      }
      UNLOCK_CLIENTS;

      usleep(REFEREE_PERIOD_US);
   }
}

// Discrete IIR, single-pole lowpass filter. Used in scafd only by the referee
// using a priori profiling.  Time constant rc is expected to be the same
// across calls. Inputs x and dt are the data and time interval, respectively.
float lowpass(float x, float dt, float rc){
   static float yp = 0.5;
   float alpha = dt / (rc + dt);
   yp = alpha * x + (1.0-alpha) * yp;
   return yp;
}

void text_scoreboard_body(void* data){
   while(1){
      RD_LOCK_CLIENTS;
      text_print_clients();
      UNLOCK_CLIENTS;
      sleep(text_interface);
   }
}

void curses_scoreboard_body(void* data){
   WINDOW *wnd;
   wnd = initscr();
   noecho();
   clear();
   refresh();

   while(1){
      RD_LOCK_CLIENTS;
      curses_print_clients();
      UNLOCK_CLIENTS;
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

void lookout_body(void* data){
   while(1){
      // Just keep this global up to date.
      bg_utilization = proc_get_cpus_used();
   }
}

int main(int argc, char **argv){

    int c;
    while( (c = getopt(argc, argv, "ct:heqbl:")) != -1){
       switch(c){
          case 'q':
             curses_interface = 0;
             text_interface = 0;
             break;
          case 'e':
             equipartitioning = 1;
             break;
          case 'c':
             curses_interface = 1;
             text_interface = 0;
             break;
          case 't':
             text_interface = atoi(optarg);
             curses_interface = 0;
             break;
          case 'b':
             nobgload = 1;
             break;
          case 'l':
             logfilename = optarg;;
             break;
          case 'h':
          default:
             printf("Usage: %s [-h] [-q] [-e] [-b] [-l logfile] [-c] [-t n]\n\t-h\tdisplay this message\n\t-q\tbe quiet: no status interface\n\t-b\tdon't monitor background load: assume load of 0\n\t-l logfile\tlog allocations and efficiencies to logfile\n\t-e\tonly do strict equipartitioning\n\t-c\tuse a curses status interface\n\t-t n\tuse a plain text status interface, printing every n seconds\n", argv[0]);
             exit(1);
             break;
       }
    }

    max_threads = omp_get_max_threads();
    bg_utilization = proc_get_cpus_used();
    if(logfilename)
       lf = fopen(logfilename, "w");
    startuptime = rtclock();

    void (*referee_body)(void *) = equipartitioning?&equi_referee_body:&maxspeedup_referee_body;

    pthread_t referee, reaper, scoreboard, lookout;
    pthread_rwlock_init(&clients_lock, NULL);
    if(curses_interface)
       pthread_create(&scoreboard, NULL, (void *(*)(void*))&curses_scoreboard_body, NULL);
    else if(text_interface)
       pthread_create(&scoreboard, NULL, (void *(*)(void*))&text_scoreboard_body, NULL);
    pthread_create(&referee, NULL, (void *(*)(void*))referee_body, NULL);
    pthread_create(&reaper, NULL, (void *(*)(void*))&reaper_body, NULL);
    pthread_create(&lookout, NULL, (void *(*)(void*))&lookout_body, NULL);

    void *context = zmq_init (1);
    int num_clients = 0;

    //  Socket to talk to clients
    void *responder = zmq_socket (context, ZMQ_REP);
    zmq_bind (responder, SCAF_CONNECT_STRING);

    while (1) {
        //  Wait for next request from client
        zmq_msg_t request;
        zmq_msg_init (&request);
#if ZMQ_VERSION_MAJOR > 2
        int r = zmq_recvmsg (responder, &request, 0);
#else
        int r = zmq_recv (responder, &request, 0);
#endif
        //  Ignore failed recvs for now; these are usually just interruptions
        //  due to SIGWINCH
        if(r<0)
           continue;

        scaf_client_message *client_message = (scaf_client_message*)(zmq_msg_data(&request));
        // Update client bookkeeping if necessary
        int threads = perform_client_request(client_message);
        assert(threads > 0 || client_message->message == SCAF_FORMER_CLIENT);
        assert(threads < 4096);
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
