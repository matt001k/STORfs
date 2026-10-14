FROM ubuntu:24.04

VOLUME ["/storfs"]
WORKDIR /storfs
RUN cd /storfs

ARG DEBIAN_FRONTEND=noninteractive
RUN apt update --fix-missing -q
RUN apt install --no-install-recommends -y \
    software-properties-common \
    build-essential \
    git \
    symlinks \
    expect \
    curl \
    cmake \
    ruby-full \
    python3.12 \
    python3-pip \
    openssh-client \
    flex \
    bison \
    doxygen \
    astyle
RUN git clone https://github.com/ThrowTheSwitch/Ceedling.git
RUN cd Ceedling && git checkout v1.1.10 && git submodule update --init --recursive
RUN cd Ceedling && gem build ceedling.gemspec && gem install ./ceedling-1.1.0.gem --no-document
RUN pip3 install --break-system-packages gcovr==8.4

COPY ./run.sh /run.sh
RUN chmod +x /run.sh

ENTRYPOINT ["/run.sh"]
CMD ["-t"]
