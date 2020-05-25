#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -e -u -o pipefail

if [ $# != 1 ]; then
	echo "Usage: $0 VERS" 1>&2
	echo "  e.g. $0 1.0" 1>&2
	exit 2
fi

VERS=$1
PKG=fsverity-utils-$VERS

sed -E -i \
	"/\#define PACKAGE_VERSION/s/v[0-9]+(\.[0-9]+)*(-[a-z0-9]+)?/v$VERS/" \
	fsverity.c
git commit -a --signoff --message=v$VERS
git tag --sign v$VERS --message=$PKG

git archive v$VERS --prefix=$PKG/ > $PKG.tar
rm -rf $PKG
tar xf $PKG.tar
( cd $PKG && make all )
rm -r $PKG

gpg --detach-sign --armor $PKG.tar
DESTDIR=/pub/linux/kernel/people/ebiggers/fsverity-utils/v$VERS
kup mkdir $DESTDIR
kup put $PKG.tar $PKG.tar.asc $DESTDIR/$PKG.tar.gz