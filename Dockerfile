# Multi-stage image: build with GCC 13 (Ubuntu 24.04), run on the same base so
# libstdc++ matches. Ubuntu images exist for amd64 and arm64.
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake ninja-build git ca-certificates make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# FetchContent downloads happen at configure time; the whole tree is copied
# (see .dockerignore) so that optional directories such as cli/ need not exist
# for the COPY to succeed.
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --target search_server search_cli \
    && mkdir -p /out \
    && cp "$(find build -type f -name search_server | head -n 1)" /out/search_server \
    && cp "$(find build -type f -name search_cli | head -n 1)" /out/search_cli

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /out/search_server /app/search_server
COPY --from=build /out/search_cli /app/search_cli
COPY web/ /app/web/

ENV PORT=8080 \
    DATASET_PATH=/data/dataset \
    OLLAMA_URL=http://ollama:11434 \
    OLLAMA_MODEL=llama3.2:1b \
    OLLAMA_TIMEOUT_MS=15000 \
    WEB_ROOT=/app/web
EXPOSE 8080
CMD ["/app/search_server"]
