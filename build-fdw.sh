#!/bin/bash

if [ "X" == "X$GITHUB_USER" ]; then echo "Need GITHUB_USER environment variable"; exit 10; fi
if [ "X" == "X$GITHUB_PASSWORD" ]; then echo "Need GITHUB_PASSWORD environment variable"; exit 10; fi
if [ "X" == "X$GITHUB_EMAIL" ]; then echo "Need GITHUB_EMAIL environment variable"; exit 10; fi
if [ "X" == "X$HOME" ]; then echo "Need HOME environment variable"; exit 10; fi

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

exit 10

# -- make sure we have proto module built for us --
pushd src/common/proto
gyp --depth=. proto.gyp
make
if [ $? -ne 0 ]; then echo "Failed to make proto"; exit 10; fi
popd

# -- figure out the next release number --
function release {
  echo "release"
  pushd $GPCONFIG_PATH
  VERSION=`npm version patch`
  git add package.json
  if [ $? -ne 0 ]; then echo "Failed to add package.json to patch"; exit 10; fi
  git commit -m "Increased version number to $VERSION"
  if [ $? -ne 0 ]; then echo "Failed to commit patch"; exit 10; fi
  git push
  if [ $? -ne 0 ]; then echo "Failed to push to repo."; exit 10; fi
  git tag -f $VERSION
  if [ $? -ne 0 ]; then echo "Failed to tag repo"; exit 10; fi
  git push origin $VERSION
  if [ $? -ne 0 ]; then echo "Failed to push tag to repo."; exit 10; fi
  popd
  RELEASE_PATH="$HOME/build-result/virtdb-dbconfig-$VERSION"
  mkdir -p $RELEASE_PATH
  cp -R $GPCONFIG_PATH/* $RELEASE_PATH
  mkdir -p $RELEASE_PATH/lib
  pushd $RELEASE_PATH/..
  tar cvfj gpconfig-${VERSION}.tbz virtdb-dbconfig-$VERSION 
  rm -Rf virtdb-dbconfig-$VERSION
  popd
  echo $VERSION > version
}

[[ ${1,,} == "release" ]] && RELEASE=true || RELEASE=false

echo "Building node-connector"
pushd $NODE_CONNECTOR_PATH
npm install
node_modules/gulp/bin/gulp.js build
popd

echo "Building greenplum-config"
pushd $GPCONFIG_PATH
npm install
if [ $? -ne 0 ]; then echo "npm install"; exit 10; fi
echo "Node connector:"
if [ ! -e ../common/node-connector ] ; then echo "../common/node-connector doesn't exist"; exit 10; fi
ls ../common/node-connector
npm install ../common/node-connector
if [ $? -ne 0 ]; then echo "npm install ../common/node-connector failed"; exit 10; fi
echo "virtdb-connector:"
ls node_modules/virtdb-connector
node_modules/gulp/bin/gulp.js build
popd

[[ $RELEASE == true ]] && release || echo "non-release"

