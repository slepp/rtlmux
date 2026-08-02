# syntax=docker/dockerfile:1

FROM alpine:3.22 AS build

ARG TARGETARCH
ARG TARGETVARIANT

RUN apk --no-cache add bsd-compat-headers build-base libevent-dev libevent-static pkgconf

WORKDIR /app
COPY Makefile *.c *.h options.ggo ./
COPY tests tests

RUN make static \
  && strip rtlmux \
  && case "${TARGETARCH}/${TARGETVARIANT}" in \
       amd64/) name=amd64 ;; \
       arm64/) name=arm64 ;; \
       arm/v7) name=armv7 ;; \
       *) echo "Unsupported target: ${TARGETARCH}/${TARGETVARIANT}" >&2; exit 1 ;; \
     esac \
  && mkdir /out \
  && cp rtlmux "/out/rtlmux-linux-${name}"

FROM scratch AS release
COPY --from=build /out/ /

FROM scratch AS runtime
COPY --from=build /app/rtlmux /rtlmux
ENTRYPOINT ["/rtlmux"]
