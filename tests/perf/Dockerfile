FROM ubuntu:22.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential python3 wrk ca-certificates procps \
 && rm -rf /var/lib/apt/lists/*
COPY container_entrypoint.sh /usr/local/bin/perf-entrypoint
RUN chmod 755 /usr/local/bin/perf-entrypoint
WORKDIR /source
ENTRYPOINT ["/usr/local/bin/perf-entrypoint"]
CMD ["sh"]
