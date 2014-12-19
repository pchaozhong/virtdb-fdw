#!/bin/bash

if [ "X" == "X$GITHUB_USER" ]; then echo "Need GITHUB_USER environment variable"; exit 10; fi
if [ "X" == "X$GITHUB_PASSWORD" ]; then echo "Need GITHUB_PASSWORD environment variable"; exit 10; fi
if [ "X" == "X$GITHUB_EMAIL" ]; then echo "Need GITHUB_EMAIL environment variable"; exit 10; fi
if [ "X" == "X$HOME" ]; then echo "Need HOME environment variable"; exit 10; fi
if [ "X" == "X$BUILDNO" ]; then echo "Need BUILDNO environment variable"; exit 10; fi

cd $HOME

git clone --recursive https://$GITHUB_USER:$GITHUB_PASSWORD@github.com/starschema/virtdb-fdw.git virtdb-fdw
if [ $? -ne 0 ]; then echo "Failed to clone virtdb-fdw repository"; exit 10; fi
echo Creating build

echo >>$HOME/.netrc
echo machine github.com >>$HOME/.netrc
echo login $GITHUB_USER >>$HOME/.netrc
echo password $GITHUB_PASSWORD >>$HOME/.netrc
echo >>$HOME/.netrc

cd $HOME/virtdb-fdw

git --version
git config --global push.default simple
git config --global user.name $GITHUB_USER
git config --global user.email $GITHUB_EMAIL

cd virtdb-fdw
make
if [ $? -ne 0 ]; then echo "Failed to make FDW"; exit 10; fi

git tag -f $BUILDNO
if [ $? -ne 0 ]; then echo "Failed to tag repo"; exit 10; fi
git push origin $BUILDNO
if [ $? -ne 0 ]; then echo "Failed to push tag to repo."; exit 10; fi

VERSION=$BUILDNO 
RELEASE_PATH="$HOME/build-result/virtdb-fdw-$VERSION"
mkdir -p $RELEASE_PATH

cp -f virtdb_fdw.so $RELEASE_PATH/
cp -f src/virtdb_fdw.control $RELEASE_PATH/
cp -f src/virtdb_fdw--1.0.0.sql $RELEASE_PATH/

ldd virtdb_fdw.so

pushd $RELEASE_PATH/..
tar cvfj virtdb-fdw-${VERSION}.tbz virtdb-fdw-$VERSION 
rm -Rf virtdb-fdw-$VERSION
popd

