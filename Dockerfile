# ───────────────────────────────────────────────────────────────────────
# Stage 1: builder
# ───────────────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Системные зависимости
# grpc-dev включает protobuf-compiler, libprotobuf-dev, libgrpc++-dev
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    ca-certificates \
    python3-pip \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

# Conan 2
RUN pip3 install --break-system-packages conan==2.*

# Создаём профиль Conan (detect компилятор)
RUN conan profile detect --force

WORKDIR /src

# Копируем только conanfile — кэшируем слой до копирования исходников
COPY conanfile.txt .
RUN conan install . \
    --output-folder=build \
    --build=missing \
    -s compiler.cppstd=23 \
    -s build_type=Release

# Теперь копируем весь исходник
COPY . .

RUN cmake -S . -B build \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake \
    -DCMAKE_CXX_STANDARD=23 \
    && cmake --build build --parallel $(nproc)

# ───────────────────────────────────────────────────────────────────────
# Stage 2: runtime
# Минимальный образ — только runtime зависимости
# ───────────────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51 \
    libprotobuf32 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Не-root пользователь — least privilege
RUN useradd -m -u 1001 -s /bin/sh tradebot
USER tradebot
WORKDIR /app

COPY --from=builder --chown=tradebot:tradebot /src/build/finam_tradebot /app/finam_tradebot

# FINAM_SECRET_TOKEN передаётся через --env, никогда не запекаться в образ
ENTRYPOINT ["/app/finam_tradebot"]
