# Stage 1: Build the application
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
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
# -DBUILD_SHARED_LIBS=OFF forces cpr and other deps to be static
RUN mkdir build && cd build && \
    cmake -DBUILD_SHARED_LIBS=OFF .. && \
    make -j$(nproc)

# Stage 2: Create the runtime image
FROM ubuntu:22.04

# Install runtime dependencies
# We still need libssl3 because cpr/curl likely links against the system OpenSSL
RUN apt-get update && apt-get install -y \
    libssl3 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the statically linked executable from the builder stage
COPY --from=builder /app/build/kvv_aggregator .

# Copy the vehicle types data file
COPY --from=builder /app/vehicle_types.txt .

# Expose the internal port defined in main.cpp
EXPOSE 8080

# Run the application
CMD ["./kvv_aggregator"]
