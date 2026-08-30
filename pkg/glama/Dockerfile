# Glama MCP directory (glama.ai) check image — NOT required to run the tool.
#
# memory-for-ai needs NO Docker to run. This image exists only so Glama
# can build a sandbox, launch the stdio MCP server, and run its introspection
# checks, which power the directory's score badge. The same image is exercised
# locally and in CI by pkg/glama/verify.sh to guard against drift.
#
# We pull the "-portable" release asset, which is fully statically linked
# (gcc -static) — unlike the standard asset, which dynamically links glibc/
# libstdc++ and would fail on an older base. Because it's static, the runtime
# base image is arbitrary. TARGETARCH is amd64 / arm64, matching the asset names.

FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171 AS fetch
ARG TARGETARCH
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "https://github.com/LonelyTraderBay/memory-for-ai/releases/latest/download/memory-for-ai-linux-${TARGETARCH}-portable.tar.gz" \
      | tar -xz -C /tmp memory-for-ai LICENSE THIRD_PARTY_NOTICES.md \
 && chmod +x /tmp/memory-for-ai \
 && rm -rf /var/lib/apt/lists/*

FROM debian:bookworm-slim@sha256:88200866dfff7ea7f5cbcb6ec7c8a701889efe6fe859fe64d6990e4b07ea4171
COPY --from=fetch /tmp/memory-for-ai /usr/local/bin/memory-for-ai
COPY --from=fetch /tmp/LICENSE /tmp/THIRD_PARTY_NOTICES.md /usr/share/doc/memory-for-ai/
ENV MFA_CACHE_DIR=/tmp/cbm
ENTRYPOINT ["memory-for-ai"]
