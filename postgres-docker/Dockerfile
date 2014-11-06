FROM postgres:9.3

RUN mkdir /docker-entrypoint-initdb.d
ADD ./install-fdw.sh /docker-entrypoint-initdb.d/
VOLUME ["/fdw"]
