FROM espressif/esp-matter:release-v1.4.2@sha256:1c42f1d281fd84d8fe2ddf36b1e023950269307c4d84dcbd938cc76026ca59c5

# Match the firmware job in .github/workflows/validate.yml. Prepare once;
# normal builds reuse this layer and never fetch or checkout the SDK.
ENV ESPYSPA_MATTER_REVISION=4cedd4ad40821933d6cc237e80ab6604f2953fb0
ENV ESPYSPA_IDF_REVISION=4c2820d377d1375e787bcef612f0c32c1427d183
RUN git -C "$ESP_MATTER_PATH" fetch --depth 1 origin "$ESPYSPA_MATTER_REVISION" \
    && git -C "$ESP_MATTER_PATH" checkout --detach FETCH_HEAD \
    && git -C "$ESP_MATTER_PATH" submodule update --init --depth 1 \
    && test "$(git -C "$IDF_PATH" rev-parse HEAD)" = "$ESPYSPA_IDF_REVISION"

# The upstream image includes clang++, but omits its sanitizer runtime.
# Install host-test prerequisites once, never in a normal build container.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libclang-rt-18-dev \
    && rm -rf /var/lib/apt/lists/*

ENV IDF_CCACHE_ENABLE=1 CCACHE_DIR=/workspace/.local-build/ccache \
    CCACHE_BASEDIR=/workspace CCACHE_MAXSIZE=500M IDF_TARGET=esp32c6
