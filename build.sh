#!/bin/bash
echo $@

CLEAN=false
INSTALL=false

function doClean {
    echo "Starting clean at: $PWD"
    make virtdb-clean
}

function doBuild {
    make
}

function doInstall {
    echo "installing here"
}

for arg in "$@"
do
    case $arg in
    clean|--clean) CLEAN=true
	;;
    install|--install) INSTALL=true
	;;
    *) echo "Unknown parameter. Usage: $0 <clean> <install>"
	;;
    esac
done

if $CLEAN; then doClean; fi
doBuild
if $INSTALL; then doInstall; fi
exit $?
