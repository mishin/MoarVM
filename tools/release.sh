#!/bin/sh
VERSION=$1
exec >MANIFEST
echo MANIFEST
git ls-files
for submod in 3rdparty/dyncall/ 3rdparty/libuv/ 3rdparty/linenoise/; do
cd $submod
git ls-files | perl -pe "s{^}{$submod}"
cd ../..;
done
[ -d MoarVM-$VERSION ] || ln -s . MoarVM-$VERSION
perl -pe "s{^}{MoarVM-$VERSION/}" MANIFEST | tar zc -T - -f MoarVM-$VERSION.tar.gz
rm MoarVM-$VERSION
