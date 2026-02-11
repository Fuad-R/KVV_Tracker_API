# Stage 1: Build the application
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
# cmake: for build system
# build-essential: for g++ and make
# git: for FetchContent to download dependencies (Crow, json, etc.)
# libssl-dev: required by cpr and Crow (if using SSL)
# ca-certificates: to verify SSL connections during fetch
RUN apt-get update && apt-get install -y \
    cmake \
    build-essential \
    git \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the source code
COPY . .

# Create build directory and compile
# The app uses C++17 and FetchContent for dependencies
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)

# Stage 2: Create the runtime image
FROM ubuntu:22.04

# Install runtime dependencies (OpenSSL is needed for HTTPS requests to KVV)
RUN apt-get update && apt-get install -y \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the compiled executable from the builder stage
# The CMakeLists.txt defines the executable name as "kvv_aggregator"
COPY --from=builder /app/build/kvv_aggregator .

# Copy any necessary static files if needed (e.g., vehicle_types.txt if used at runtime)
COPY --from=builder /app/vehicle_types.txt .

# Expose the internal port defined in main.cpp
EXPOSE 8080

# Run the application
CMD ["./kvv_aggregator"]
