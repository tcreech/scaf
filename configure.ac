AC_INIT([scaf], [0.1], [tcreech@umd.edu])
AM_INIT_AUTOMAKE([foreign -Wall -Werror])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile src/Makefile])
#AC_CONFIG_MACRO_DIR([m4])

AC_CHECK_LIB([zmq], [zmq_init], [], [AC_MSG_ERROR([zeromq (libzmq) not found. This is a required dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line.])])

AC_CHECK_LIB([papi], [PAPI_ipc], [], [AC_MSG_WARN([PAPI (libpapi) not found. This is an optional dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line. SCAF will compile WITHOUT runtime instrumentation.])])

AC_CHECK_LIB([hwloc], [hwloc_topology_init], [], [AC_MSG_WARN([Hardware locality library (hwloc) not found. This is an optional dependence. Please add the required CPPFLAGS and LDFLAGS to your configure line. SCAF will compile WITHOUT affinity-based parallelism control.])])

AC_OUTPUT
