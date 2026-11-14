# ==============================================================================
# PC/OpenData - High-Performance Relational Engine Container
# C11 POSIX engine with authenticated native TLS networking.
# ==============================================================================

# Build stage
FROM alpine:3.19 AS builder

RUN apk add --no-cache clang musl-dev make openssl-dev openldap-dev

WORKDIR /build
COPY src/ /build/src/
COPY include/ /build/include/

# Compile the TLS-enabled C11 amalgamation
RUN clang -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -DOPENDATA_TLS \
    /build/src/opendata.c -lssl -lcrypto -lldap -llber -lm -o /build/opendata

# Runtime stage
FROM alpine:3.19

RUN apk add --no-cache ca-certificates tzdata openssl libldap

# Create non-root database user and directories
RUN addgroup -S opendata && adduser -S opendata -G opendata && \
    mkdir -p /data /backups && \
    chown -R opendata:opendata /data /backups

COPY --from=builder /build/opendata /usr/local/bin/opendata

USER opendata
WORKDIR /data

VOLUME ["/data", "/backups"]

EXPOSE 7338
ENV OPENDATA_TLS_CA=/run/tls/ca.pem OPENDATA_TLS_SERVER_NAME=localhost

# Probe TLS without reopening the already-locked database.
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD openssl s_client -connect 127.0.0.1:7338 -brief -verify_return_error \
        -verify_hostname "$OPENDATA_TLS_SERVER_NAME" -CAfile "$OPENDATA_TLS_CA" </dev/null >/dev/null 2>&1

ENTRYPOINT ["/usr/local/bin/opendata"]
CMD ["--database", "/data/production.pcod", "--backup-dir", "/backups", "--backup-interval", "60", "--tcps", "--bind", "0.0.0.0", "--port", "7338", "--cert", "/run/tls/server.pem", "--key", "/run/tls/server-key.pem"]
