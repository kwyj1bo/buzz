# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUZZ_BUILD_TESTS=OFF \
 && cmake --build build \
 && cmake --install build --prefix /opt/buzz --strip

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3t64 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --create-home --home-dir /var/lib/buzz buzz \
 && mkdir -p /var/lib/buzz/certs \
 && chown -R buzz:buzz /var/lib/buzz
COPY --from=build /opt/buzz/bin/ /usr/local/bin/
USER buzz
WORKDIR /var/lib/buzz
VOLUME ["/var/lib/buzz/certs"]
EXPOSE 7000/tcp 7000/udp
ENTRYPOINT ["buzz-server"]
CMD ["--port", "7000", "--gen-cert"]
