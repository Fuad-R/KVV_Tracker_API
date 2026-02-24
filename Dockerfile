# Stage 1: Build the application
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies + PostgreSQL dev libraries
RUN apt-get update && apt-get install -y \
    cmake \
    build-essential \
    git \
    libssl-dev \
    ca-certificates \
    libpq-dev \
    postgresql-client \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the source code
COPY . .

# Build the project
RUN mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc)


# Stage 2: Runtime image
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies (including PostgreSQL runtime lib)
RUN apt-get update && apt-get install -y \
    libssl3 \
    ca-certificates \
    libpq5 \
    libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy compiled binary
COPY --from=builder /app/build/kvv_aggregator .

# Copy libcpr shared library from builder
COPY --from=builder /app/build/_deps/cpr-build/cpr/libcpr.so.1 /usr/local/lib/
RUN ldconfig

# Copy optional data file
COPY --from=builder /app/vehicle_types.txt .

# Expose port
EXPOSE 8080

# Run
CMD ["./kvv_aggregator"]