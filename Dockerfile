from centos:centos6

# Install make, gcc 4.8 and dev stuff
RUN yum -y groupinstall 'Development Tools'
RUN curl http://people.centos.org/tru/devtools-2/devtools-2.repo > /etc/yum.repos.d/devtools-2.repo
RUN yum install -y devtoolset-2-gcc devtoolset-2-binutils
RUN yum install -y devtoolset-2-gcc-c++
RUN yum -y install tar
RUN yum install -y which

# Install postgresql
RUN rpm -ivh http://yum.postgresql.org/9.3/redhat/rhel-6-x86_64/pgdg-centos93-9.3-1.noarch.rpm
RUN yum -y install postgresql93 postgresql93-server postgresql93-libs postgresql93-contrib postgresql93-devel
ENV PATH /opt/rh/devtoolset-2/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/pgsql-9.3/bin

# Create user for build and PG
RUN mkdir /home/testuser
WORKDIR /home/testuser

# Install 0mq
RUN curl http://download.opensuse.org/repositories/home:/fengshuo:/zeromq/CentOS_CentOS-6/home:fengshuo:zeromq.repo > /etc/yum.repos.d/zeromq.repo
RUN yum install -y zeromq-devel

# Install Protocol Buffers
RUN curl https://protobuf.googlecode.com/svn/rc/protobuf-2.6.0.tar.gz > protobuf-2.6.0.tar.gz
RUN tar -xzvf protobuf-2.6.0.tar.gz
WORKDIR /home/testuser/protobuf-2.6.0
RUN ./configure
RUN make
RUN make install
ENV PKG_CONFIG_PATH /usr/local/lib/pkgconfig

CMD mkdir -p /fdw
WORKDIR /fdw
