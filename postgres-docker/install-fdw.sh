mkdir -p /usr/lib/postgresql/9.3/lib
mkdir -p /usr/share/postgresql/9.3/extension/
cp /fdw/virtdb_fdw.so /usr/lib/postgresql/9.3/lib
cp /fdw/virtdb_fdw.control /usr/share/postgresql/9.3/extension/
cp /fdw/virtdb_fdw--1.0.0.sql /usr/share/postgresql/9.3/extension/
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/fdw/lib
echo "Install finished" >> /fdw/test.log
