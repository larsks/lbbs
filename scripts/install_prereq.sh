#/bin/sh

# == Packages
# Debian: apt-get
# Fedora: yum/dnf (RPM)

# -- Core
PACKAGES_DEBIAN="build-essential git" # make, git
PACKAGES_FEDORA="git gcc binutils-devel"

# used by libopenarc, libetpan
PACKAGES_DEBIAN="$PACKAGES_DEBIAN make automake pkg-config libtool m4"

# OpenSSL
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libssl-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA openssl-devel"

PACKAGES_DEBIAN="$PACKAGES_DEBIAN libncurses-dev" # ncurses

PACKAGES_DEBIAN="$PACKAGES_DEBIAN libcrypt-dev" # crypt_r

# <curl/curl.h> - cURL, OpenSSL variant
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libcurl4-openssl-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA libcurl-devel"

PACKAGES_DEBIAN="$PACKAGES_DEBIAN binutils-dev" # <bfd.h>

# <sys/capability.h>
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libcap-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA libcap-devel"

# <uuid/uuid.h>
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libuuid1 uuid-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA libuuid-devel"

# <bsd/string.h>
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libbsd-dev"
PACKAGES_FEDORA="$PACKAGES_DEBIAN libbsd-devel"

# <histedit.h>, <readline/history.h>
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libedit-dev libreadline-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA libedit-devel readline-devel"

# libssh (net_ssh)
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libssh-dev"
# net_ssh, which requires objdump to test for symbol existence... thanks a lot, libssh
PACKAGES_DEBIAN="$PACKAGES_DEBIAN binutils" # objdump
PACKAGES_FEDORA="$PACKAGES_FEDORA libssh-devel"

# lirc (mod_irc_client)
scripts/lirc.sh

# MariaDB (MySQL) dev headers (mod_mysql, mod_mysql_auth)
# mariadb-server is also required to run a local DBMS, but this is not required for either compilation or operation.
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libmariadb-dev libmariadb-dev-compat"
PACKAGES_FEDORA="$PACKAGES_FEDORA mariadb105-devel"

# LMDB (mod_lmdb)
PACKAGES_DEBIAN="$PACKAGES_DEBIAN liblmdb-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA lmdb-devel"

# <magic.h> (mod_http)
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libmagic-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA file-devel"

# OpenDKIM (mod_smtp_filter_dkim)
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libopendkim-dev"

# mod_oauth
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libjansson-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA jansson-devel"

# mod_mimeparse
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libglib2.0-dev libgmime-3.0-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA glib2-devel"

# mod_smtp_filter_arc
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libmilter-dev"
PACKAGES_FEDORA="$PACKAGES_FEDORA sendmail-milter-devel"

# mod_smtp_filter_spf
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libspf2-dev"
# MISSING: RPM package

# mod_smtp_filter_dmarc
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libopendmarc-dev"
# MISSING: RPM package

# mod_sieve
PACKAGES_DEBIAN="$PACKAGES_DEBIAN libsieve2-dev"
# MISSING: RPM package

OS=$(( uname -s ))
if [ -f /etc/debian_version ]; then
	apt-get update
	apt-get install -y $PACKAGES_DEBIAN
elif [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
	dnf install -y $PACKAGES_FEDORA
else
	printf "Could not install %s packages (unsupported distro?)\n" "$OS"
fi

# == Source Install

# libdiscord (mod_discord)
scripts/libdiscord.sh

# libwss (net_ws)
scripts/libwss.sh

# mod_slack (also depends on libwss)
scripts/libslackrtm.sh

# mod_smtp_filter_arc
scripts/libopenarc.sh

# libetpan (mod_webmail): the package no longer suffices, since we patch the source.
#PACKAGES_DEBIAN="$PACKAGES_DEBIAN libetpan-dev"
scripts/libetpan.sh

# doxygen only:
#PACKAGES_DEBIAN="$PACKAGES_DEBIAN doxygen graphviz"
