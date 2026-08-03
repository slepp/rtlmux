#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>

#include <event2/event.h>
#include <event2/thread.h>
#include <event2/http.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/util.h>

#include <arpa/inet.h>

#include <pthread.h>

#include "config.h"

#include "slog.h"

#include "rtlmux.h"

#include <sys/queue.h>

struct event_base *event_base = NULL;
struct bufferevent *serverConnection = NULL;

unsigned long dataBlocks = 0;
unsigned long dataBlocksSize = 0;
struct rtlData {
  uint32_t references;
  uint32_t len;
  uint8_t *data;
};

#define CLIENT_READY 2
#define CLIENT_INIT 4
struct client {
  LIST_ENTRY(client) peer;
  struct bufferevent *bev;
  union {
    struct sockaddr sa;
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
  };
  struct {
    uint64_t in;
    uint64_t out;
    uint64_t dropped;
    uint64_t droppedCount;
  } data;
  time_t connected;
  uint32_t flags;
};

static LIST_HEAD(clienthead, client) clients = LIST_HEAD_INITIALIZER(clients);

static struct client *addClient(struct bufferevent *bev) {
  uint32_t clientFlags = CLIENT_INIT;
  
  struct client *client = (struct client *)calloc(1, sizeof(struct client));
  if(client == NULL)
    return NULL;
  
  client->bev = bev;
  client->flags = clientFlags;
  client->data.in = client->data.out = 0;
  client->connected = time(NULL);
  
  LIST_INSERT_HEAD(&clients, client, peer);
  
  return client;
}

static void removeClient(struct client *client) {
  if(!client)
    return;
  
  LIST_REMOVE(client, peer);
  free(client);

  if(config.delayed && LIST_EMPTY(&clients)) {
    slog(LOG_INFO, SLOG_INFO, "Last user disconnected.");
    timeToExit = config.restart ? 2 : 1;
  }
}

void releaseDataRef(const void *d, unsigned long len, void *ptr) {
  (void)d;
  (void)len;
  struct rtlData *data = (struct rtlData *)ptr;
  --data->references;
  if(data->references == 0) {
    dataBlocks--;
    dataBlocksSize -= data->len;
    free(data); // This is a single malloc for both the data and header
  }
}

int sendDataToAllClients(struct rtlData *data) {
  struct client *client;
  LIST_FOREACH(client, &clients, peer) {
    if(client->flags == CLIENT_READY) {
      struct evbuffer *ev = bufferevent_get_output(client->bev);
      if(evbuffer_get_length(ev) > 4*1024*1024) { // If we've already buffered 4MByte, then start dropping frames
        client->data.dropped += data->len;
        client->data.droppedCount ++;
        continue;
      }
      if(evbuffer_add_reference(ev, data->data, data->len, releaseDataRef, data) == 0) {
        ++data->references;
        client->data.out += data->len;
      } else {
        client->data.dropped += data->len;
        client->data.droppedCount ++;
      }
    }
  }
  return data->references;
}

static void logCB(int severity, const char *msg) {
  int level;
  int flag;
  switch(severity) {
    case EVENT_LOG_DEBUG: level = LOG_DEBUG; flag = SLOG_DEBUG; break;
    case EVENT_LOG_MSG: level = LOG_INFO; flag = SLOG_INFO; break;
    case EVENT_LOG_WARN: level = LOG_WARN; flag = SLOG_WARN; break;
    case EVENT_LOG_ERR: level = LOG_ERROR; flag = SLOG_ERROR; break;
    default: level = LOG_LIVE; flag = LOG_LIVE; break;
  }
  
  slog(level, flag, "%s", msg);
}

struct serverInfo {
  enum { SERVER_NEW, SERVER_CONNECTED, SERVER_DISCONNECTED } state;
  char magic[4];
  uint32_t tuner_type;
  uint32_t tuner_gain_count;
  struct {
    uint32_t value;
    uint8_t set;
  } params[0xd]; // Store all the parameters as a simple command array
  struct {
    uint64_t in;
    uint64_t out;
  } data;
} serverInfo;

static void readyClient(struct client *client) {
  uint8_t header[12];
  memcpy(header, serverInfo.magic, 4);
  memcpy(header + 4, &serverInfo.tuner_type, 4);
  memcpy(header + 8, &serverInfo.tuner_gain_count, 4);

  if(bufferevent_write(client->bev, header, sizeof(header)) == 0) {
    client->data.out += 12;
    client->flags = CLIENT_READY;
  }
}

static void readyWaitingClients(void) {
  struct client *client;
  LIST_FOREACH(client, &clients, peer) {
    if(client->flags == CLIENT_INIT)
      readyClient(client);
  }
}

static void signalEventCB(evutil_socket_t signal, short events, void *ctx) {
  (void)events;
  (void)ctx;
  slog(LOG_INFO, SLOG_INFO, "Received signal %d.", (int)signal);
  timeToExit = 1;
}

static void serverErrorEventCB(struct bufferevent *, short, void *);
static void serverReadCB(struct bufferevent *, void *);
static void connectToServerSoon(void);

static void disconnectServer(struct bufferevent *bev) {
  if(serverConnection == bev)
    serverConnection = NULL;
  bufferevent_free(bev);
}

static void connectToServer(void) {
  if(serverConnection != NULL || timeToExit)
    return;

  slog(LOG_INFO, SLOG_INFO, "Starting connection lookup for %s:%d", config.host, config.port);
  serverConnection = bufferevent_socket_new(event_base, -1, BEV_OPT_CLOSE_ON_FREE);
  if(serverConnection == NULL) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not allocate server connection.");
    timeToExit = 1;
    return;
  }

  bufferevent_setcb(serverConnection, serverReadCB, NULL, serverErrorEventCB, NULL);
  bufferevent_setwatermark(serverConnection, EV_READ, 1, 0);
  bufferevent_enable(serverConnection, EV_READ|EV_WRITE);
  if(bufferevent_socket_connect_hostname(serverConnection, NULL, AF_UNSPEC, config.host, config.port) != 0) {
    slog(LOG_ERROR, SLOG_ERROR, "Could not start connection to %s:%d", config.host, config.port);
    disconnectServer(serverConnection);
    connectToServerSoon();
    return;
  }
  slog(LOG_INFO, SLOG_INFO, "Started to connect to %s:%d", config.host, config.port);
}

static void connectToServerCB(int a, short b, void *arg) {
  (void)a;
  (void)b;
  (void)arg;
  connectToServer();
}

static void connectToServerSoon(void) {
  struct timeval tv;

  if(timeToExit)
    return;

  tv.tv_sec = 1;
  tv.tv_usec = 0;

  if(event_base_once(event_base, -1, EV_TIMEOUT, connectToServerCB, NULL, &tv) != 0) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not schedule server reconnect.");
    timeToExit = 1;
  }
}

static void serverReadCB(struct bufferevent *bev, void *ctx) {
  (void)ctx;
  struct rtlData *data;
  struct evbuffer *ev = bufferevent_get_input(bev);
  
  if(serverInfo.state == SERVER_NEW) {
    if(evbuffer_get_length(ev) < 12)
      return;

    serverInfo.data.in += evbuffer_remove(ev, serverInfo.magic, 4);
    serverInfo.data.in += evbuffer_remove(ev, &serverInfo.tuner_type, 4);
    serverInfo.data.in += evbuffer_remove(ev, &serverInfo.tuner_gain_count, 4);
    if(serverInfo.magic[0] == 'R' && serverInfo.magic[1] == 'T' && serverInfo.magic[2] == 'L' && serverInfo.magic[3] == '0') {
      serverInfo.state = SERVER_CONNECTED;
      bufferevent_setwatermark(bev, EV_READ, 16384, 0);
      slog(LOG_INFO, SLOG_INFO, "Connected to server.");
      readyWaitingClients();
    } else { // Failed to receive the magic header
      slog(LOG_ERROR, SLOG_ERROR, "Failed to receive magic header from server.");
      disconnectServer(bev);
      serverInfo.state = SERVER_NEW;
      if (config.delayed) {
        timeToExit = config.restart ? 2 : 1;
      } else {
        connectToServerSoon();
      }
      return;
    }
    // Send stored and set parameters on reconnect
    int i;
    for(i = 0; i < 0xd; i++) {
      if(serverInfo.params[i].set) {
        struct command cmd;
        cmd.cmd = (uint8_t)(i+1);
        cmd.param = serverInfo.params[i].value;
        slog(LOG_INFO, SLOG_INFO, "Sending command %d with param %u", cmd.cmd, ntohl(cmd.param));
        serverInfo.data.out += sizeof(cmd);
        bufferevent_write(bev, &cmd, sizeof(cmd));
      }
    }
  }
  
  size_t availLen = evbuffer_get_length(ev);
  
  if(availLen == 0) // We may not have data, so return
    return;
  
  if(availLen > 256*1024)
    availLen = 256*1024; // Limit our input sizes to 256k chunks

  data = (struct rtlData *)malloc(sizeof(struct rtlData) + availLen);
  if(data == NULL) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not allocate input data buffer.");
    timeToExit = 1;
    return;
  }
  memset(data, 0, sizeof(struct rtlData));
  data->data = (uint8_t *)data + sizeof(struct rtlData);
  data->references = 0;
  data->len = (uint32_t)bufferevent_read(bev, data->data, availLen);
  serverInfo.data.in += data->len;
  
  if(sendDataToAllClients(data) == 0) {
    // No one was listening
    free(data);
  } else {
    dataBlocks++;
    dataBlocksSize += data->len;    
  }
}

static void serverErrorEventCB(struct bufferevent *bev, short events, void *ctx) {
  (void)ctx;
  if (events & BEV_EVENT_ERROR) {
    int error = EVUTIL_SOCKET_ERROR();
    slog(LOG_ERROR, SLOG_ERROR, "Error from server side bufferevent: %s", evutil_socket_error_to_string(error));
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    disconnectServer(bev);
    slog(LOG_INFO, SLOG_INFO, "Disconnecting server.");
    serverInfo.state = SERVER_NEW;
    connectToServerSoon();
  }
}

static void errorEventCB(struct bufferevent *bev, short events, void *ctx) {
  if (events & BEV_EVENT_ERROR) {
    int error = EVUTIL_SOCKET_ERROR();
    slog(LOG_ERROR, SLOG_ERROR, "Error from bufferevent: %s", evutil_socket_error_to_string(error));
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    struct client *client = (struct client *)ctx;
    char ipBuf[128];
    if(client->sa.sa_family == AF_INET)
      evutil_inet_ntop(client->sa.sa_family, &client->sin.sin_addr, ipBuf, 128);
    else if(client->sa.sa_family == AF_INET6)
      evutil_inet_ntop(client->sa.sa_family, &client->sin6.sin6_addr, ipBuf, 128);
    else
      snprintf(ipBuf, 128, "from unknown address");
    slog(LOG_INFO, SLOG_INFO, "Disconnecting client %s", ipBuf);
    bufferevent_free(bev);
    removeClient(client);
  }
}

void serverSendCommand(struct command cmd) {
  if(cmd.cmd < 1 || cmd.cmd > 0xd) {
    slog(LOG_WARN, SLOG_WARN, "Ignoring invalid command: %u", cmd.cmd);
    return;
  }

  serverInfo.params[cmd.cmd-1].value = cmd.param;
  serverInfo.params[cmd.cmd-1].set = 1;
  if(serverConnection != NULL && serverInfo.state == SERVER_CONNECTED) {
    slog(LOG_LIVE, SLOG_DEBUG, "Sending command to server: %d: %u", cmd.cmd, ntohl(cmd.param));
    serverInfo.data.out += sizeof(cmd);
    bufferevent_write(serverConnection, &cmd, sizeof(cmd));
  }
}

#define RTL_FREQUENCY 0x01
#define RTL_SAMPLE_RATE 0x02
#define RTL_GAIN_MODE 0x03
#define RTL_GAIN 0x04
#define RTL_FREQ_CORRECTION 0x05
#define RTL_STAGE_GAIN 0x06
#define RTL_TEST_MODE 0x07
#define RTL_AGC_MODE 0x08
#define RTL_DIRECT_SAMPLING 0x09
#define RTL_OFFSET_TUNING 0x0a
#define RTL_XTAL 0x0b
#define RTL_TUNER_XTAL 0x0c
#define RTL_GAIN_BY_INDEX 0x0d

static void clientReadCB(struct bufferevent *bev, void *ctx) {
  struct command cmd;
  struct client *client = (struct client *)ctx;
  struct evbuffer *ev = bufferevent_get_input(bev);
  while(evbuffer_get_length(ev) >= sizeof(cmd)) {
    evbuffer_remove(ev, &cmd, sizeof(cmd));
    client->data.in += sizeof(cmd);
    slog(LOG_INFO, SLOG_INFO, "Read from client: %x", cmd.cmd);
    
    switch(cmd.cmd) {
      case RTL_FREQUENCY: // Frequency
      slog(LOG_INFO, SLOG_INFO, "Set frequency: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_SAMPLE_RATE: // Sample rate
      slog(LOG_INFO, SLOG_INFO, "Set sample rate: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN_MODE: // Gain mode
      slog(LOG_INFO, SLOG_INFO, "Set gain mode: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN: // Set Gain
      slog(LOG_INFO, SLOG_INFO, "Set gain: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_FREQ_CORRECTION: // Set freq correction
      slog(LOG_INFO, SLOG_INFO, "Set freq correction: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_STAGE_GAIN: // Stage Gain
      slog(LOG_INFO, SLOG_INFO, "Set stage gain: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_TEST_MODE: // Test mode
      slog(LOG_INFO, SLOG_INFO, "Set test mode: %u", ntohl(cmd.param));
      break;
      case RTL_AGC_MODE: // AGC mode
      slog(LOG_INFO, SLOG_INFO, "Set AGC mode: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_DIRECT_SAMPLING: // Direct sampling
      slog(LOG_INFO, SLOG_INFO, "Set direct sampling: %u", ntohl(cmd.param));
      break;
      case RTL_OFFSET_TUNING: // Offset tuning
      slog(LOG_INFO, SLOG_INFO, "Set offset tuning: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_XTAL: // RTL Xtal
      slog(LOG_INFO, SLOG_INFO, "Set RTL xtal: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_TUNER_XTAL: // Tuner Xtal
      slog(LOG_INFO, SLOG_INFO, "Set tuner xtal: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN_BY_INDEX: // Gain by index
      slog(LOG_INFO, SLOG_INFO, "Set gain by index: %u", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      default: // Ignore it
      slog(LOG_INFO, SLOG_INFO, "Ignored client command: %x", cmd.cmd);
    }
  }
}

static void connectCB(struct evconnlistener *listener,
    evutil_socket_t sock, struct sockaddr *addr, int len, void *ptr) {
    (void)ptr;
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = bufferevent_socket_new(
            base, sock, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
    if(bev == NULL) {
      slog(LOG_ERROR, SLOG_ERROR, "Could not allocate client connection.");
      evutil_closesocket(sock);
      return;
    }

    if (config.delayed && serverConnection == NULL) {
      slog(LOG_INFO, SLOG_INFO, "Connection to server triggered.");
      connectToServer();
    }

    struct client *client = addClient(bev);
    if(client == NULL) {
      slog(LOG_ERROR, SLOG_ERROR, "Could not allocate client state.");
      bufferevent_free(bev);
      return;
    }
    if((size_t)len > sizeof(client->sin6))
      len = sizeof(client->sin6);
    memcpy(&client->sa, addr, (size_t)len);
    char ipBuf[128];
    if(client->sa.sa_family == AF_INET)
      evutil_inet_ntop(client->sa.sa_family, &client->sin.sin_addr, ipBuf, 128);
    else if(client->sa.sa_family == AF_INET6)
      evutil_inet_ntop(client->sa.sa_family, &client->sin6.sin6_addr, ipBuf, 128);
    else
      snprintf(ipBuf, 128, "from unknown address");
    slog(LOG_INFO, SLOG_INFO, "Connection from client %s%s", ipBuf, LIST_NEXT(client,peer) == NULL ? " (first!)" : "");
    bufferevent_setcb(bev, clientReadCB, NULL, errorEventCB, client);
    bufferevent_setwatermark(bev, EV_WRITE, 0, 4*1024*1024); // Limit output to 4MB
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    if(serverInfo.state == SERVER_CONNECTED)
      readyClient(client);
}

static void dumpClients(struct evhttp_request *req, void *arg) {
  (void)arg;
  struct evbuffer *evb = NULL;

  evb = evbuffer_new();
  if(evb == NULL) {
    evhttp_send_error(req, 500, "Could not allocate response");
    return;
  }

  evbuffer_add_printf(evb, "{\"server\":{\"dataIn\":%" PRIu64 ",\"dataOut\":%" PRIu64 "},\"clients\":[",
    serverInfo.data.in, serverInfo.data.out);
  struct client *client;
  LIST_FOREACH(client, &clients, peer) {
    char ipBuf[128];
    if(client->sa.sa_family == AF_INET)
      evutil_inet_ntop(client->sa.sa_family, &client->sin.sin_addr, ipBuf, 128);
    else if(client->sa.sa_family == AF_INET6)
      evutil_inet_ntop(client->sa.sa_family, &client->sin6.sin6_addr, ipBuf, 128);
    else
      snprintf(ipBuf, 128, "from unknown address");
    evbuffer_add_printf(evb, "{\"client\":{\"host\":\"%s\",\"port\":%u},\"dataIn\":%" PRIu64 ",\"dataOut\":%" PRIu64 ",\"dropped\":{\"size\":%" PRIu64 ",\"count\":%" PRIu64 "},\"connected\":%lld}",
      ipBuf, ntohs(client->sa.sa_family == AF_INET ? client->sin.sin_port : client->sin6.sin6_port),
      client->data.in,
      client->data.out,
      client->data.dropped,
      client->data.droppedCount,
      (long long)client->connected
    );
    if(LIST_NEXT(client, peer) != NULL) {
      evbuffer_add_printf(evb, ",");
    }
  }
  evbuffer_add_printf(evb, "]}");
  
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
  evhttp_send_reply(req, 200, "OK", evb);
  evbuffer_free(evb);
}

void *serverThread(void *arg) {  
  (void)arg;
  struct evconnlistener *clientListener = NULL;
  struct evhttp *http = NULL;
  struct event *sigtermEvent = NULL;
  struct event *sigintEvent = NULL;
  memset(&serverInfo, 0, sizeof(serverInfo));
  dataBlocks = 0;
  dataBlocksSize = 0;
  
  slog(LOG_INFO, SLOG_INFO, "Starting server thread.");
  
  LIST_INIT(&clients);
  
  event_set_log_callback(logCB);
  
  if(evthread_use_pthreads() != 0) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not initialize libevent threading.");
    timeToExit = 1;
    return NULL;
  }
  
  // Libevent loop
  event_base = event_base_new();
  if(event_base == NULL) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not allocate event base.");
    timeToExit = 1;
    return NULL;
  }

  sigtermEvent = evsignal_new(event_base, SIGTERM, signalEventCB, NULL);
  sigintEvent = evsignal_new(event_base, SIGINT, signalEventCB, NULL);
  if(sigtermEvent == NULL || sigintEvent == NULL ||
      event_add(sigtermEvent, NULL) != 0 || event_add(sigintEvent, NULL) != 0) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not configure signal events.");
    timeToExit = 1;
    goto cleanup;
  }
  
  struct sockaddr_in6 sa;
  socklen_t salen = sizeof(sa);
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_addr = in6addr_any;
  sa.sin6_port = htons(config.clientPort);
  
  clientListener = evconnlistener_new_bind(event_base, connectCB, NULL,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
    (struct sockaddr *)&sa, salen);
  if(!clientListener) {
    timeToExit = 1;
    slog(LOG_FATAL, SLOG_FATAL, "Could not listen on the client streaming port.");
    goto cleanup;
  }
  
  slog(LOG_INFO, SLOG_INFO, "Listening for clients on port %d", config.clientPort);

  if (!config.delayed) {
    connectToServer();
  } else {
    slog(LOG_INFO, SLOG_INFO, "Connection to server delayed.");
  }

  struct evhttp_bound_socket *handle;
  http = evhttp_new(event_base);
  if(http == NULL) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not allocate HTTP server.");
    timeToExit = 1;
    goto cleanup;
  }

  evhttp_set_cb(http, "/stats.json", dumpClients, "clients");
  
  handle = evhttp_bind_socket_with_handle(http, "::", config.clientPort + 1);

  if(!handle) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not bind HTTP listener.");
    timeToExit = 1;
    goto cleanup;
  }

  int loopCounter = 0;
  while(!timeToExit) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    
    event_base_loopexit(event_base, &tv);
    event_base_dispatch(event_base);
    
    if((++loopCounter%600) == 0) {
      loopCounter = 0;
      if(dataBlocks > 0)
        slog(LOG_INFO, SLOG_INFO, "Maintaining %lu data buffers, total of %lu bytes.", dataBlocks, dataBlocksSize);
    }
  }
  
cleanup:
  while(LIST_FIRST(&clients) != NULL) {
    struct client *client = LIST_FIRST(&clients);
    LIST_REMOVE(client, peer);
    bufferevent_free(client->bev);
    free(client);
  }

  if (serverConnection != NULL) {
    disconnectServer(serverConnection);
    serverInfo.state = SERVER_DISCONNECTED;
    slog(LOG_INFO, SLOG_INFO, "Disconnecting from server.");
  }

  if(clientListener != NULL)
    evconnlistener_free(clientListener);
  if(http != NULL)
    evhttp_free(http);
  if(sigtermEvent != NULL)
    event_free(sigtermEvent);
  if(sigintEvent != NULL)
    event_free(sigintEvent);
  if(event_base != NULL) {
    event_base_free(event_base);
    event_base = NULL;
  }

  slog(LOG_INFO, SLOG_INFO, "End of server thread.");
  return NULL;
}
