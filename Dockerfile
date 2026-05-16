# FIX[DEVOPS-1]: Multi-stage image keeps compilers out of runtime and runs as non-root.
FROM gcc:13-bookworm AS build
WORKDIR /src
RUN apt-get update && apt-get install -y --no-install-recommends cmake libssl-dev ca-certificates && rm -rf /var/lib/apt/lists/*
COPY . .
RUN cmake -S . -B build -DBUILD_TESTING=OFF && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 ca-certificates && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home --shell /usr/sbin/nologin tsh
WORKDIR /app
COPY --from=build /src/build/tsh_server /usr/local/bin/tsh_server
COPY --from=build /src/build/tsh_client /usr/local/bin/tsh_client
COPY --from=build /src/build/tsh_worker /usr/local/bin/tsh_worker
COPY --from=build /src/build/tsh_spine_server /usr/local/bin/tsh_spine_server
COPY --from=build /src/build/tsh_spine_agent /usr/local/bin/tsh_spine_agent
COPY --from=build /src/build/tsh_spine_submit /usr/local/bin/tsh_spine_submit
USER 10001
EXPOSE 4444 5555 7443 7444 8080
ENTRYPOINT ["/usr/local/bin/tsh_server"]
