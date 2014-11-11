function usage {
    echo "$0 [--clean|-c|clean] [--install=<docker-container-name>]"
}

CLEAN=""
INSTALL=false

while [ "$1" != "" ]; do
    PARAM=`echo $1 | awk -F= '{print $1}'`
    VALUE=`echo $1 | awk -F= '{print $2}'`
    case $PARAM in
        -h | --help)
            usage
            exit
            ;;
        --clean|-c|clean)
            CLEAN="--clean"
            ;;
        --install)
            INSTALL=true
            CONTAINER=$VALUE
            ;;
        --port)
            PORT=$VALUE
            ;;
        *)
            echo "ERROR: unknown parameter \"$PARAM\""
            usage
            exit 1
            ;;
    esac
    shift
done

function install {
    cp -f virtdb_fdw.so postgres-docker/
    cp -f src/virtdb_fdw.control postgres-docker/
    cp -f src/virtdb_fdw--1.0.0.sql postgres-docker/
    docker kill $CONTAINER 2> /dev/null
    docker rm $CONTAINER 2> /dev/null
    docker run --name $CONTAINER -p=0.0.0.0:$PORT:5432 -d -v $PWD/postgres-docker:/fdw postgres-with-fdw
    echo "Successfully installed fdw to $CONTAINER"
}

docker rm fdw-builder
docker run -t --rm -v $PWD:/fdw --name fdw-builder fdw-build /fdw/build.sh $CLEAN
$INSTALL && install
