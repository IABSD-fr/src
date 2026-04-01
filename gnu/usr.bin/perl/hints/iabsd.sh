# hints/iabsd.sh
#
# hints file for IABSD; based on OpenBSD hints by Todd Miller
# Edited to allow Configure command-line overrides by
#  Andy Dougherty <doughera@lafayette.edu>
#
# To build with distribution paths, use:
#	./Configure -des -Dopenbsd_distribution=defined
#

# IABSD has a better malloc than perl...
test "$usemymalloc" || usemymalloc='n'

# malloc wrap works
case "$usemallocwrap" in
'') usemallocwrap='define' ;;
esac

# Currently, vfork(2) is not a real win over fork(2).
usevfork="$undef"

# 64 bit time_t
cppflags="$cppflags -DBIG_TIME"

#
# Not all platforms support dynamic loading...
# For the case of "$openbsd_distribution", the hints file
# needs to know whether we are using dynamic loading so that
# it can set the libperl name appropriately.
# Allow command line overrides.
#
ARCH=`arch | sed 's/^IABSD.//'`
case "${ARCH}" in
vax)
	test -z "$usedl" && usedl=$undef
	;;
*)
	test -z "$usedl" && usedl=$define
	# We use -fPIC here because -fpic is *NOT* enough for some of the
	# extensions like Tk on some platforms (ie: sparc)
	PICFLAG=-fPIC
	if [ -e /usr/share/mk/bsd.own.mk ]; then
		PICFLAG=`make -f /usr/share/mk/bsd.own.mk -V PICFLAG`
	fi
	cccdlflags="-DPIC ${PICFLAG} $cccdlflags"
	ld=${cc:-cc}
	lddlflags="-shared ${PICFLAG} $lddlflags"
	libswanted=`echo $libswanted | sed 's/ dl / /'`

	# We need to force ld to export symbols on ELF platforms.
	# Without this, dlopen() is crippled.
	ELF=`${cc:-cc} -dM -E - </dev/null | grep __ELF__`
	test -n "$ELF" && ldflags="-Wl,-E $ldflags"
	;;
esac

# IABSD doesn't need libcrypt
libswanted=`echo $libswanted | sed 's/ crypt / /'`

# IABSD doesn't need linking to libutil
libswanted=`echo $libswanted | sed 's/ util / /'`

# Configure can't figure this out non-interactively
d_suidsafe=$define

# Allow a command-line override, such as -Doptimize=-g
case "${ARCH}" in
    alpha)
	ccflags="-fno-tree-ter $ccflags"
	;;
    vax)
	ccflags="-DUSE_PERL_ATOF=0 $ccflags"
	;;
esac
test "$optimize" || optimize='-O2'

# This script UU/usethreads.cbu will get 'called-back' by Configure
# after it has prompted the user for whether to use threads.
cat > UU/usethreads.cbu <<'EOCBU'
case "$usethreads" in
$define|true|[yY]*)
	ccflags="-pthread $ccflags"
	ldflags="-pthread $ldflags"
	;;
*)
	libswanted=`echo $libswanted | sed 's/ pthread / /'`
esac
EOCBU

# When building in the IABSD tree we use different paths
# This is only part of the story, the rest comes from config.over
case "$openbsd_distribution" in
''|$undef|false) ;;
*)
	# We put things in /usr, not /usr/local
	prefix='/usr'
	prefixexp='/usr'
	sysman='/usr/share/man/man1'
	libpth='/usr/lib'
	glibpth='/usr/lib'
	# Local things, however, do go in /usr/local
	siteprefix='/usr/local'
	siteprefixexp='/usr/local'
	# Ports installs non-std libs in /usr/local/lib so look there too
	locincpth=''
	loclibpth=''
	# Link perl with shared libperl
	if [ "$usedl" = "$define" -a -r $src/shlib_version ]; then
		useshrplib=true
		libperl=`. $src/shlib_version; echo libperl.so.${major}.${minor}`
	fi
	;;
esac

# newlocale() may still have issues
#d_newlocale="$undef"

# libc on this platform always keeps these categories in the C locale
ccflags="$ccflags -DNO_LOCALE_NUMERIC -DNO_LOCALE_COLLATE -DNO_LOCALE_MONETARY -DNO_LOCALE_TIME -DNO_LOCALE_MESSAGES"

# And the only possible mismatched LC_CTYPE locale is C.UTF-8 (as only it and
# plain C are legal for this category).  All other categories deal only in
# ASCII characters which are a subset of C.UTF-8.
ccflags="$ccflags -DLIBC_HANDLES_MISMATCHED_CTYPE"

# Locale support is not that complete yet
ccflags="-DNO_LOCALE_NUMERIC -DNO_LOCALE_COLLATE $ccflags"

# Bogus values in _Thread_local variables in shared objects. See GH #19109
d_thread_local=undef

# end
