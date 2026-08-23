# MusicPack test stack image: server + CLI tools + web client.
# Copyright (c) 2026, The MusicPack Development Team
# SPDX-License-Identifier: BSD-3-Clause

# ---- 1. WASM decoder module (Emscripten) --------------------------------
FROM emscripten/emsdk:3.1.50 AS wasm
WORKDIR /src
COPY . .
RUN emcmake cmake -S . -B build-wasm -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build-wasm --target musepack_wasm -j"$(nproc)"

# ---- 2. Web client (static SPA build) -----------------------------------
FROM node:22-bookworm-slim AS web
WORKDIR /src/web
COPY web/package.json web/package-lock.json* ./
RUN npm ci
COPY web/ ./
COPY demo/ /src/demo/
ENV WASM_BUILD=/src/build-wasm
COPY --from=wasm /src/build-wasm /src/build-wasm
RUN npm run build

# ---- 3. Native build: server + CLI + codec tools -------------------------
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config libmicrohttpd-dev python3 curl \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ARG BUILD_TESTS=OFF
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DMPC_BUILD_TESTS=${BUILD_TESTS} \
      -DMPC_BUILD_MPCGAIN=OFF -DMPC_BUILD_MPCCHAP=OFF \
 && cmake --build build -j"$(nproc)" \
 && find build \( -name 'libmusicpack.so*' -o -name 'libmusepack.so*' \) \
      -exec cp {} /usr/local/lib/ \; && ldconfig

# ---- 4. Runtime image ----------------------------------------------------
FROM debian:bookworm-slim AS runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends libmicrohttpd12 ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -m musicpack
COPY --from=build /src/build/server/musicpack-server /usr/local/bin/
COPY --from=build /src/build/musicpack/musicpack       /usr/local/bin/
COPY --from=build /src/build/mpcenc/mpcenc             /usr/local/bin/
COPY --from=build /src/build/mpcdec/mpcdec             /usr/local/bin/
COPY --from=web /src/web/app/dist /srv/web
COPY --from=build /usr/local/lib/ /usr/local/lib/
RUN ldconfig
USER musicpack
ENV MUSICPACK_LISTEN=0.0.0.0 MUSICPACK_PORT=8080
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s \
  CMD curl -fsS http://127.0.0.1:8080/api/v1/health || exit 1
ENTRYPOINT ["/usr/local/bin/musicpack-server"]
CMD ["serve", "--static-dir", "/srv/web"]
