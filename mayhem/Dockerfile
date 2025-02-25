FROM ubuntu AS builder

RUN apt update -y && DEBIAN_FRONTEND=noninteractive apt install -y build-essential software-properties-common lsb-release clang lld wget git
RUN apt clean all
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"
RUN apt update -y && DEBIAN_FRONTEND=noninteractive apt install -y cmake

COPY . /luau
WORKDIR /luau
# RUN make -j8 config=release luau luau-analyze
# RUN make -j8
RUN CC=clang CXX=clang++ LDFLAGS=-fuse-ld=lld cmake -DCMAKE_CXX_FLAGS=-std=c++17 -B build
WORKDIR /luau/build
RUN make -j8

FROM ubuntu
COPY --from=builder /luau/build/luau /
COPY --from=builder /luau/build/luau-analyze /
COPY --from=builder /luau/build/luau-ast /
COPY --from=builder /luau/build/luau-bytecode /
COPY --from=builder /luau/build/luau-compile /
# COPY --from=builder /luau/build/luau-reduce /
COPY --from=builder /luau/build/fuzz/fuzz-proto /
COPY --from=builder /luau/build/fuzz/fuzz-prototest /
