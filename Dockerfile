# SPDX-License-Identifier: Apache-2.0
#
# The hosted PACE analyzer.
#
# The Dockerfile lives at the repository root on purpose. The build context is
# the whole repository -- the service depends on the analyzer crate by path --
# and Render's default docker path is ./Dockerfile, so keeping it there means
# the deployment works from the manifest and from the default with no override
# to fight. The root .dockerignore keeps the context to the two crates.
#
# Two stages, because the toolchain is three gigabytes and the service is a few
# megabytes.

FROM rust:1.99.1-slim-trixie AS build

WORKDIR /src

# The analyzer is the product; the service is a transport in front of it.
COPY pace-analyzer/ pace-analyzer/
COPY service/ service/

# --locked so the build uses the committed lockfile rather than resolving
# fresh versions at deploy time: the artifact that gets deployed should be the
# one that was tested.
RUN cargo build --release --locked --manifest-path service/Cargo.toml

# ---------------------------------------------------------------------------

FROM debian:trixie-slim

# The service reads a trace and writes a response. It needs no shell, no
# package manager and no root.
RUN useradd --create-home --uid 10001 pace

COPY --from=build /src/service/target/release/pace-service /usr/local/bin/pace-service

USER pace
ENV PORT=8080
EXPOSE 8080

# Render passes PORT and expects the process to listen on it.
CMD ["/usr/local/bin/pace-service"]
