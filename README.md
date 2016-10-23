RTL TCP Client Multiplexer / Relay
==================================

[![Build Status](https://git.vocti.ca/slepp/rtlmux/badges/master/build.svg)](https://git.vocti.ca/slepp/rtlmux/builds)

This is a simple server to connec to an rtl_tcp server and allow multiple clients
to attach to the same receiver. It relays the ID information to the clients,
and allows all clients to send commands for configuration to the rtl_tcp server.

It provides status output of the server and clients on the client port + 1, at
the URL /stats.json, ie. http://localhost:7879/stats.json when `-l` is set to 7878.

Usage
=====

Usage takes up to 3 command line options:

* `-h host` Server host to connect to
* `-p port` Server port to connect to
* `-l port` Listening port for clients to connect to, HTTP status port will be
  equal to this plus one
