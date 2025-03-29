# Stage 1: Build the application using Alpine
FROM alpine:latest AS builder
LABEL stage=builder

# Install necessary build tools for Alpine
# build-base includes gcc, make, libc-dev (for headers like pthread.h)
RUN apk add --no-cache \
    build-base \
    cmake \
    pkgconfig

# Set the working directory
WORKDIR /app

# Copy the CMakeLists.txt file first (leverages Docker cache)
COPY CMakeLists.txt ./

# Copy the rest of the source code
COPY . .

# Create a build directory and build the project
RUN mkdir build && \
    cd build && \
    cmake .. && \
    make

# Stage 2: Create the final runtime image based on Alpine
FROM alpine:latest

# Alpine doesn't have /usr/local/bin in PATH by default for non-login shells
# Placing it in /usr/bin is common, or add /usr/local/bin to PATH
# Let's put it in /usr/bin for simplicity here.
WORKDIR /app

# Copy only the compiled http_server executable from the build stage
COPY --from=builder /app/build/http_server /usr/bin/http_server

# (Optional) If you also need the 'test' executable in the final image:
# COPY --from=builder /app/build/test /usr/bin/test

# Add non-root user/group for security
RUN addgroup -S appgroup && adduser -S appuser -G appgroup

# Switch to non-root user
USER appuser

# Expose the port your http_server listens on (Replace 8080 if different)
EXPOSE 8080

# Set the default command to run when the container starts
CMD ["http_server"]