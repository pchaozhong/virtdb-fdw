FROM virtdb-build:centos6-base

USER virtdb-demo
WORKDIR /home/virtdb-demo
ENV HOME /home/virtdb-demo
ENV PATH $HOME/postgres-install/bin:$HOME/protobuf-install/bin:$HOME/node-install/bin:$HOME/node-install/lib/node_modules/npm/node_modules/node-gyp/gyp/:$PATH
ENV PKG_CONFIG_PATH /home/virtdb-demo/protobuf-install/lib/pkgconfig:/home/virtdb-demo/libzmq-install/lib/pkgconfig:$PKG_CONFIG_PATH
ADD build-fdw.sh /home/virtdb-demo/build-fdw.sh
USER root
RUN chmod a+x /home/virtdb-demo/build-fdw.sh
RUN chown 10012:10012 /home/virtdb-demo/build-fdw.sh
USER virtdb-demo


