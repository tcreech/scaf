AC_INIT([scaf], [0.1], [tcreech@umd.edu])
AM_INIT_AUTOMAKE([foreign -Wall -Werror])
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_CONFIG_FILES([Makefile src/Makefile])
#AC_CONFIG_MACRO_DIR([m4])

AC_ARG_WITH([zeromq],
            AS_HELP_STRING([--with-zeromq], [Specify the prefix of your zeromq installation]))

ZMQ_LOCATION=/usr/local/

AS_IF([test "x$with_zeromq" != "x"],
      [ZMQ_LOCATION="$with_zeromq"],
      [ZMQ_LOCATION=/usr/local/])
AC_SUBST(ZMQ_LOCATION)

AC_OUTPUT
