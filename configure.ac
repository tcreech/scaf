AC_INIT([scaf], [m4_esyscmd([git describe --always --dirty --long 2> /dev/null | tr -d \\n])], [tcreech@umd.edu])
AM_INIT_AUTOMAKE([foreign -Wall -Werror dist-bzip2 dist-xz])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile src/Makefile src/scafwrap], [chmod +x src/scafwrap])
AC_CONFIG_MACRO_DIR([m4])
AM_CONDITIONAL(FORFREEBSD, test `uname -s` = "FreeBSD")

AC_CHECK_LIB([zmq], [zmq_sendmsg], [], [AC_MSG_ERROR([zeromq (libzmq) 3.0.0 or newer not found. This is a required dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line.])])

AC_CHECK_LIB([papi], [PAPI_ipc], [], [AC_MSG_WARN([PAPI (libpapi) not found. This is an optional dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line. SCAF will compile WITHOUT runtime instrumentation.])])

AC_CHECK_LIB([hwloc], [hwloc_topology_init], [], [AC_MSG_WARN([Hardware locality library (hwloc) not found. This is an optional dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line. SCAF will compile WITHOUT affinity-based parallelism control.])])

AC_MSG_CHECKING([for __progname])
AC_LINK_IFELSE([AC_LANG_PROGRAM[
    extern const char* __progname;
    int foo(void) {
        return __progname == (void*)0;
    }
]],
[AC_MSG_RESULT([yes])
AC_DEFINE([HAVE_PROGNAME], [1], [Define to 1 if you have the '__progname' symbol])],
[AC_MSG_RESULT([no])
AC_MSG_WARN(can't find __progname: won't be able to do malleability whitelist)])
AC_OUTPUT
