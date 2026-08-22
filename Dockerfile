FROM postgres:18-bookworm AS build

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        postgresql-server-dev-18 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/bloompg
COPY . .
RUN make clean \
    && make -j2 \
    && make install

FROM postgres:18-bookworm

COPY --from=build /usr/lib/postgresql/18/lib/bloompg.so /usr/lib/postgresql/18/lib/bloompg.so
COPY --from=build /usr/share/postgresql/18/extension/bloompg* /usr/share/postgresql/18/extension/
COPY examples/docker-init.sql /docker-entrypoint-initdb.d/10-bloompg-demo.sql
