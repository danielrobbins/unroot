FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        busybox-static \
        libsubid-dev \
        python3-pytest \
        uidmap \
    && rm -rf /var/lib/apt/lists/*

RUN useradd --create-home unroot \
    && printf 'unroot:1000000:65536\n' >> /etc/subuid \
    && printf 'unroot:1000000:65536\n' >> /etc/subgid \
    && install -d -o unroot -g unroot /source

WORKDIR /source
USER unroot
ENV HOME=/home/unroot

COPY --chown=unroot:unroot . .
RUN make clean cli test UNROOT_UTIL_LIBSUBID=1
