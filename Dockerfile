FROM ubuntu

ENV DEBIAN_FRONTEND="noninteractive" TZ="America/New_York"
RUN apt-get update && apt-get install -y dkms gcc perl make libelf-dev bison flex debhelper
RUN apt-get install -y linux-headers-5.8.0-33-generic

ADD . /usr/src/r750-1.2.10.1

RUN dkms add -m r750 -v 1.2.10.1
RUN dkms mkdeb -m r750 -v 1.2.10.1 --source-only
RUN cd /usr/src/r750-1.2.10.1 && KERNELDIR=/lib/modules/5.8.0-33-generic/build make -C product/r750/linux
#RUN dkms build -k 5.8.0-33-generic -m r750 -v 1.2.10.1
#WORKDIR /usr/src/r750-1.2.10.1
#RUN ./install.sh

