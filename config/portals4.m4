AC_DEFUN([ACX_PORTALS4],[
  AC_ARG_VAR([PORTALS4_CFLAGS], [C compiler flags for Portals4])
  AC_ARG_VAR([PORTALS4_LIBS], [linker flags for Portals4])
  if test "x$with_portals4" != xyes; then
    PORTALS4_CFLAGS="-I$with_portals4/include $PORTALS4_CFLAGS"
    if test -z "$PORTALS4_LIBS"; then
      for portals4_libdir in lib64 lib lib/`$CC -dumpmachine`; do
        if test -f "$with_portals4/$portals4_libdir/libportals.so" -o -f "$with_portals4/$portals4_libdir/libportals.a"; then
          PORTALS4_LIBS="-L$with_portals4/$portals4_libdir -lportals"
          break
        fi
      done
      if test -z "$PORTALS4_LIBS"; then
        AC_MSG_ERROR([No Portals4 library found under $with_portals4; set PORTALS4_LIBS explicitly])
      fi
    fi
  fi
  if test -z "$PORTALS4_LIBS"; then
    PORTALS4_LIBS=-lportals
  fi
  portals4_save_CPPFLAGS=$CPPFLAGS
  portals4_save_LIBS=$LIBS
  CPPFLAGS="$CPPFLAGS $PORTALS4_CFLAGS"
  LIBS="$PORTALS4_LIBS $LIBS"
  AC_MSG_CHECKING([whether Portals4 headers and library are usable])
  AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <portals4.h>]], [[
    ptl_handle_ni_t ni;
    ptl_ni_limits_t limits = {0};
    int ret = PtlInit();
    ret += PtlNIInit(PTL_IFACE_DEFAULT, PTL_NI_PHYSICAL | PTL_NI_NO_MATCHING,
                     PTL_PID_ANY, &limits, &limits, &ni);
    ret += PtlNIFini(ni);
    PtlFini();
    return ret;
  ]])], [HAVE_PORTALS4=1], [HAVE_PORTALS4=0])
  CPPFLAGS=$portals4_save_CPPFLAGS
  LIBS=$portals4_save_LIBS
  if test "$HAVE_PORTALS4" = 1; then
    AC_MSG_RESULT([yes])
    AC_SUBST([ac_lib_portals4], [$PORTALS4_LIBS])
  else
    AC_MSG_RESULT([no])
    AC_MSG_ERROR([Portals4 requested but unusable; set PORTALS4_CFLAGS and PORTALS4_LIBS or use --with-portals4=PREFIX])
  fi
])

# Optional XPMEM path for Portals4. Probe by compiling/linking only: the
# configure host need not have the kernel module loaded (or be the target).
AC_DEFUN([ACX_PORTALS4_XPMEM],[
  AC_ARG_WITH([xpmem],
    [AS_HELP_STRING([--with-xpmem@<:@=PREFIX@:>@],
      [enable the Portals4 XPMEM local path (default: no)])],
    [with_xpmem=$withval], [with_xpmem=no])
  AC_ARG_VAR([XPMEM_CFLAGS], [C compiler flags for XPMEM])
  AC_ARG_VAR([XPMEM_LIBS], [linker flags for XPMEM])
  ac_lib_xpmem=
  HAVE_XPMEM=0
  if test "x$with_xpmem" != xno; then
    if test "x$HAVE_PORTALS4" != x1; then
      AC_MSG_ERROR([--with-xpmem requires --with-portals4])
    fi
    AC_LANG_PUSH([C])
    AC_MSG_CHECKING([whether the target supports the Portals4 XPMEM path])
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __x86_64__
#error Portals4 XPMEM currently requires x86-64 notification polling
#endif
    ]], [[]])], [xpmem_target_supported=yes], [xpmem_target_supported=no])
    AC_MSG_RESULT([$xpmem_target_supported])
    if test "$xpmem_target_supported" != yes; then
      AC_MSG_ERROR([Portals4 XPMEM currently requires x86-64; other targets need acquire notification polling in the common runtime])
    fi

    if test "x$with_xpmem" != xyes; then
      if test -z "$XPMEM_CFLAGS"; then
        if test ! -f "$with_xpmem/include/xpmem.h"; then
          AC_MSG_ERROR([No xpmem.h found under $with_xpmem; set XPMEM_CFLAGS explicitly])
        fi
        XPMEM_CFLAGS="-I$with_xpmem/include"
      fi
      if test -z "$XPMEM_LIBS"; then
        xpmem_triplet=`$CC -dumpmachine 2>/dev/null`
        for xpmem_libdir in lib64 lib lib/$xpmem_triplet; do
          if test -f "$with_xpmem/$xpmem_libdir/libxpmem.so" -o -f "$with_xpmem/$xpmem_libdir/libxpmem.a"; then
            XPMEM_LIBS="-L$with_xpmem/$xpmem_libdir -lxpmem"
            break
          fi
        done
        if test -z "$XPMEM_LIBS"; then
          AC_MSG_ERROR([No XPMEM library found under $with_xpmem; set XPMEM_LIBS explicitly])
        fi
      fi
    fi
    if test -z "$XPMEM_LIBS"; then
      XPMEM_LIBS=-lxpmem
    fi
    xpmem_save_CPPFLAGS=$CPPFLAGS
    xpmem_save_LIBS=$LIBS
    CPPFLAGS="$XPMEM_CFLAGS $CPPFLAGS"
    LIBS="$XPMEM_LIBS $LIBS"
    AC_MSG_CHECKING([whether XPMEM headers and library are usable])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <stddef.h>
#include <stdint.h>
#include <xpmem.h>
    ]], [[
      xpmem_segid_t segid = xpmem_make(NULL, XPMEM_MAXADDR_SIZE,
          XPMEM_PERMIT_MODE, (void *)(uintptr_t)0600);
      xpmem_apid_t apid = xpmem_get(segid, XPMEM_RDWR, XPMEM_PERMIT_MODE, NULL);
      struct xpmem_addr address = { .apid = apid, .offset = 0 };
      void *mapping = xpmem_attach(address, 4096, NULL);
      int ret = xpmem_detach(mapping);
      ret += xpmem_release(apid);
      ret += xpmem_remove(segid);
      return ret;
    ]])], [HAVE_XPMEM=1], [HAVE_XPMEM=0])
    CPPFLAGS=$xpmem_save_CPPFLAGS
    LIBS=$xpmem_save_LIBS
    AC_LANG_POP([C])
    if test "$HAVE_XPMEM" != 1; then
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([XPMEM requested but unusable; set XPMEM_CFLAGS and XPMEM_LIBS or use --with-xpmem=PREFIX])
    fi
    AC_MSG_RESULT([yes])
    AC_DEFINE([GPI2_PORTALS4_XPMEM], [1],
      [Define to enable the Portals4 XPMEM local communication path.])
    ac_lib_xpmem=$XPMEM_LIBS
    options="$options XPMEM"
  fi
  AC_SUBST([ac_lib_xpmem])
  AM_CONDITIONAL([WITH_XPMEM], [test "$HAVE_XPMEM" = 1])
])
