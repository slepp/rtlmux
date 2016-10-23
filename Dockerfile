FROM alpine

RUN apk --no-cache add libevent

COPY Makefile *.c *.h options.ggo /app/

WORKDIR /app

RUN apk --no-cache add --virtual build-dependencies build-base libevent-dev bsd-compat-headers \
  && make \
  && apk del build-dependencies

CMD ["/app/rtlmux"]
