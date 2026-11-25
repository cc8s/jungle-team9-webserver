FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends build-essential python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN mkdir -p uploads_storage && chmod 700 uploads_storage && make clean && make
EXPOSE 8000
CMD ["./webserver", "--worker", "4", "--queue", "64", "--max", "128", "--buffer", "32"]
