#ifndef SOLARIS_TRACE_UTILS_H
#define SOLARIS_TRACE_UTILS_H 1

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <procfs.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>
#define MIN_STR_LEN 1024

// Declarations

typedef struct {
   long cmd;
   union {
      long flags;
      sigset_t signals;
      sysset_t syscalls;
      fltset_t faults;
      siginfo_t siginfo;
   } arg;
} pctl_t;

static inline void __sol_write_proc_ctl(const pid_t pid, void* buf, size_t len);

static inline void __sol_proc_force_stop_wait(const pid_t pid);

static inline void __sol_proc_stop_wait(const pid_t pid);

static inline void __sol_proc_force_stop_nowait(const pid_t pid);

static inline void __sol_proc_run_clearsigs(const pid_t pid);

static inline void __sol_proc_run_clearsyscalls(const pid_t pid);

static inline void __sol_proc_setsig(const pid_t pid, int signum);

static inline void __sol_proc_run(const pid_t pid);

static inline void __sol_proc_trace_sigs(const pid_t pid);

static inline void __sol_proc_trace_syscalls(const pid_t pid);

static inline void __sol_proc_notrace(const pid_t pid);

static inline pstatus_t* __sol_get_proc_status (const pid_t pid);

static inline psinfo_t* __sol_get_proc_info (const pid_t pid);

// Inline helpers

inline void __sol_write_proc_ctl(const pid_t pid, void* buf, size_t len){
   char *pattern = "/proc/%d/ctl";
   char filename[MIN_STR_LEN];
   int fd;

   snprintf (filename, MIN_STR_LEN, pattern, pid);

   fd = open (filename, O_WRONLY);
   if (fd == -1){
      printf("Failed to open control file for pid %d (%s)!\n", pid, filename);
      kill(pid, SIGTERM);
      exit(1);
   }

   int err;
   if((err = write(fd, buf, len)) != len){
      printf("Failed to write control message for pid %d (to %s)! Wrote %d/%d bytes. (errno=%s)\n", pid, filename, err, len, strerror(errno));
      kill(pid, SIGTERM);
      exit (1);
   }
   close(fd);
}

inline void __sol_proc_force_stop_nowait(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCDSTOP;
   size = sizeof (long);

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_force_stop_wait(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCSTOP;
   size = sizeof (long);

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_stop_wait(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCWSTOP;
   size = sizeof (long);

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_run_clearsigs(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCRUN;
   ctl.arg.flags = PRCSIG;
   size = sizeof (long) * 2;

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_run_clearsyscalls(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCRUN;
   ctl.arg.flags = PRSABORT;
   size = sizeof (long) * 2;

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_run(const pid_t pid){
   pctl_t ctl;
   size_t size;
   int err;

   ctl.cmd = PCRUN;
   ctl.arg.flags = 0;
   size = sizeof (long) * 2;

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_trace_sigs(const pid_t pid){
   pctl_t ctl;

   size_t size;
   int err;

   ctl.cmd = PCSTRACE;
   premptyset(&ctl.arg.signals);
   praddset(&ctl.arg.signals, SIGALRM);
   size = sizeof (long) + sizeof (sigset_t);

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_trace_syscalls(const pid_t pid){
   pctl_t ctl;

   size_t size;
   int err;

   ctl.cmd = PCSENTRY;
   premptyset(&ctl.arg.syscalls);
   prfillset(&ctl.arg.syscalls);

   // These are ``safe'' syscalls SCAF will never care about. This is handled
   // differently than in Linux: Linux stops on all syscalls and does nothing
   // on safe calls. Solaris doesn't stop on safe calls as per below. This
   // allows the Solaris implementation to be a bit faster for code with many
   // syscalls, which is good because there tends to be more code like this on
   // SPARC. (Calling tons of SYS_brk, for example.)
   // TODO: There are surely manymany more of safe syscalls which can be added
   //       here as discovered.
   prdelset(&ctl.arg.syscalls, SYS_sigprocmask);
   prdelset(&ctl.arg.syscalls, SYS_sigaction);
   prdelset(&ctl.arg.syscalls, SYS_nanosleep);
   prdelset(&ctl.arg.syscalls, SYS_lwp_sigmask);
   prdelset(&ctl.arg.syscalls, SYS_close);
   prdelset(&ctl.arg.syscalls, SYS_fstat64);
   prdelset(&ctl.arg.syscalls, SYS_schedctl);
   prdelset(&ctl.arg.syscalls, SYS_ioctl);
   prdelset(&ctl.arg.syscalls, SYS_brk);

   size = sizeof (long) + sizeof (sysset_t);

   __sol_write_proc_ctl(pid, &ctl, size);
}

inline void __sol_proc_notrace(const pid_t pid){
   pctl_t ctl;

   size_t size;
   int err;

   ctl.cmd = PCSTRACE;
   premptyset(&ctl.arg.signals);
   size = sizeof (long) + sizeof (sigset_t);

   __sol_write_proc_ctl(pid, &ctl, size);

   ctl.cmd = PCSENTRY;
   premptyset(&ctl.arg.syscalls);
   size = sizeof (long) + sizeof (sysset_t);

   __sol_write_proc_ctl(pid, &ctl, size);
}

static inline pstatus_t* __sol_get_proc_status (const pid_t pid){
   char *pattern = "/proc/%d/status";
   char filename[MIN_STR_LEN];
   int fd;
   static pstatus_t proc;

   memset (&proc, 0, sizeof (proc));
   snprintf (filename, MIN_STR_LEN, pattern, pid);

   fd = open (filename, O_RDONLY);
   if (fd == -1)
      return NULL;

   read (fd, (void *) &proc, sizeof (proc));

   close (fd);

   return &proc;
}

static inline psinfo_t* __sol_get_proc_info (const pid_t pid){
   char *pattern = "/proc/%d/psinfo";
   char filename[MIN_STR_LEN];
   int fd;
   static psinfo_t proc;

   memset (&proc, 0, sizeof (proc));
   snprintf (filename, MIN_STR_LEN, pattern, pid);

   fd = open (filename, O_RDONLY);
   if (fd == -1)
      return NULL;

   read (fd, (void *) &proc, sizeof (proc));

   close (fd);

   return &proc;
}

static inline void __sol_proc_setsig(const pid_t pid, int signum){
   pctl_t ctl;

   size_t size;
   int err;

   ctl.cmd = PCSSIG;
   ctl.arg.siginfo.si_signo = signum;
   ctl.arg.siginfo.si_code = SI_USER;
   ctl.arg.siginfo.si_errno = 0;
   size = sizeof (long) + sizeof (siginfo_t);

   __sol_write_proc_ctl(pid, &ctl, size);
}

#endif //SOLARIS_TRACE_UTILS_H
