# RTL TCP Client Multiplexer / Relay

`rtlmux` connects to one [`rtl_tcp`](https://osmocom.org/projects/rtl-sdr/wiki/Rtl-sdr) server and lets multiple network clients share its sample stream. Clients receive the normal RTL TCP header and sample data, and supported tuner commands are forwarded to the upstream receiver.

The service also provides a small JSON status endpoint with connection and traffic statistics.

> [!IMPORTANT]
> RTL TCP has no authentication or encryption, and every connected client can change the shared tuner configuration. Run `rtlmux` only on a trusted LAN or behind an appropriate firewall or VPN.

## Quick start

Download the static binary for your Linux system from the [latest GitHub release](https://github.com/slepp/rtlmux/releases/latest):

```sh
# x86_64 / amd64
curl -LO https://github.com/slepp/rtlmux/releases/latest/download/rtlmux-linux-amd64
chmod +x rtlmux-linux-amd64

# Connect to rtl_tcp at 192.168.1.50:1234 and listen for clients on port 7878
./rtlmux-linux-amd64 -h 192.168.1.50 -p 1234 -l 7878
```

Release binaries are available for:

| File | Platform |
| --- | --- |
| `rtlmux-linux-amd64` | 64-bit Intel and AMD Linux |
| `rtlmux-linux-arm64` | 64-bit ARM Linux |
| `rtlmux-linux-armv7` | 32-bit ARMv7 Linux, including WEB-888 |

Point RTL TCP clients at the machine running `rtlmux`, port `7878` in the example above. Status is available at <http://localhost:7879/stats.json>.

## Usage

```text
rtlmux [OPTIONS]

  -h, --host=ADDRESS  rtl_tcp server address (default: localhost)
  -p, --port=PORT     rtl_tcp server port (default: 1234)
  -l, --listen=PORT   client listening port (default: 7878)
  -d, --delayed       connect upstream on demand and exit after the last client
  -r, --restart       restart after the last client disconnects
  -V, --version       print version and exit
      --help          print complete help and exit
```

The HTTP status endpoint uses the port immediately after the client port. For example, `-l 7878` uses port `7879` for HTTP.

With `-d` alone, `rtlmux` exits after the last client disconnects. For a long-running standby service, use `-d -r` together:

```sh
./rtlmux-linux-armv7 -h 192.168.1.50 -p 1234 -l 7878 -d -r
```

This keeps the upstream connection idle until the first client arrives, then closes and resets it after the last client disconnects.

## Docker

Multi-architecture images are published to [Docker Hub](https://hub.docker.com/r/slepp/rtlmux) for amd64, arm64, and ARMv7:

```sh
docker run --rm --network host slepp/rtlmux:latest \
  -h 192.168.1.50 -p 1234 -l 7878 -d -r
```

Version tags and `latest` are published from Git version tags. The `edge` image follows the repository's primary branch.

## Build from source

Install a C compiler, `make`, `pkg-config`, the libevent development files, and Python 3. On Debian or Ubuntu:

```sh
sudo apt-get install build-essential libevent-dev pkg-config python3
make
make test
sudo make install
```

`make static` creates a statically linked binary when static libevent libraries are installed. The Docker build provides a reproducible Alpine/musl static build:

```sh
docker build --target runtime -t rtlmux .
```

## Releases

Pushing a tag such as `v1.1.0` builds static Linux binaries for amd64, arm64, and ARMv7, creates checksums, and publishes a GitHub release.

Docker Hub publishing uses the repository secrets `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN`. Primary-branch builds publish `edge`; version tags publish the version and `latest`.
