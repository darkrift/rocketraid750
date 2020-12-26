FROM ubuntu
ARG KERNEL_VERSION=5.8.0-33-generic

ENV DEBIAN_FRONTEND="noninteractive" TZ="America/New_York"
RUN apt-get update && apt-get install -y dkms gcc perl make libelf-dev bison flex debhelper
RUN apt-get install -y linux-headers-${KERNEL_VERSION}

ADD . /usr/src/r750-1.2.10.1

RUN dkms add -m r750 -v 1.2.10.1
RUN dkms mkdeb -m r750 -v 1.2.10.1 --source-only
RUN cd /usr/src/r750-1.2.10.1 && KERNELDIR=/lib/modules/${KERNEL_VERSION}/build make -C product/r750/linux
#RUN dkms build -k ${KERNEL_VERSION} -m r750 -v 1.2.10.1
#WORKDIR /usr/src/r750-1.2.10.1
#RUN ./install.sh

