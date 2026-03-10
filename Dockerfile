# Stage 1: Build the application
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
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

# Copy only CMakeLists.txt first to fetch and build dependencies (cached layer)
COPY CMakeLists.txt .

# Create a dummy main.cpp so cmake configure + dependency fetch succeeds
RUN mkdir build && cd build && \
    echo "int main(){return 0;}" > ../main.cpp && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) || true

# Now copy the real source files
COPY main.cpp .
COPY vehicle_types.txt .

# Rebuild with real source (dependencies are already built and cached)
RUN cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && \
    make install

# Stage 2: Runtime image
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    ca-certificates \
    libpq5 \
    libcurl4 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for running the application
RUN groupadd -r appuser && useradd -r -g appuser -d /app -s /sbin/nologin appuser

WORKDIR /app

# Copy binary
COPY --from=builder /app/build/kvv_aggregator .

# Copy installed libraries from builder
COPY --from=builder /usr/local/lib /usr/local/lib

# Ensure linker can find libs
RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/local.conf && ldconfig

# Optional data file
COPY --from=builder /app/vehicle_types.txt .

# Set ownership and switch to non-root user
RUN chown -R appuser:appuser /app
USER appuser

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8080/health

CMD ["./kvv_aggregator"]