# GNU 68HC11/68HC12 toolchain: GCC 3.3.6 with the m68hc1x patches (release 3.1), binutils 2.18
# and newlib 1.18, the packages Debian 7 "wheezy" shipped, installed on a current Debian
# (wheezy's own glibc no longer runs on current kernels; its binaries run on a newer glibc).
FROM debian:bookworm-slim
ARG POOL=http://archive.debian.org/debian/pool/main
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates make python3 \
 && cd /tmp \
 && curl -fsSLO $POOL/g/gcc-m68hc1x/gcc-m68hc1x_3.3.6+3.1+dfsg-3+b2_amd64.deb \
 && curl -fsSLO $POOL/b/binutils-m68hc1x/binutils-m68hc1x_2.18-3.2_amd64.deb \
 && curl -fsSLO $POOL/n/newlib/newlib-m68hc1x_1.18.0-6.2_all.deb \
 && dpkg -i --force-depends *.deb \
 && rm -f *.deb && apt-get purge -y curl && apt-get autoremove -y && rm -rf /var/lib/apt/lists/*
WORKDIR /src
