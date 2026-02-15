#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
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

// Define this to enable thread safety around the lists
//#define THREADED

#ifndef THREADED
#define pthread_rwlock_wrlock(a)
#define pthread_rwlock_rdlock(a)
#define pthread_rwlock_unlock(a)
#endif

struct event_base *event_base = NULL;
struct bufferevent *serverConnection = NULL;

unsigned long dataBlocks = 0;
unsigned long dataBlocksSize = 0;
struct rtlData {
  LIST_ENTRY(rtlData) next;
  uint32_t references;
  uint32_t len;
  uint8_t *data;
};
static LIST_HEAD(rtlDataHead, rtlData) rtlDataList = LIST_HEAD_INITIALIZER(rtlDataList);
static pthread_rwlock_t rtlDataLock;

#define CLIENT_UNKNOWN 1
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
static pthread_rwlock_t clientLock;

static struct client *addClient(struct bufferevent *bev, void *ptr) {
  uint32_t clientFlags = CLIENT_INIT;
  
  struct client *client = (struct client *)calloc(1, sizeof(struct client));
  
  client->bev = bev;
  client->flags = clientFlags;
  client->data.in = client->data.out = 0;
  client->connected = time(NULL);
  
  pthread_rwlock_wrlock(&clientLock);
  LIST_INSERT_HEAD(&clients, client, peer);
  pthread_rwlock_unlock(&clientLock);
  
  return client;
}

static void removeClient(struct client *client) {
  if(!client)
    return;
  
  pthread_rwlock_wrlock(&clientLock);
  LIST_REMOVE(client, peer);
  free(client);
  pthread_rwlock_unlock(&clientLock);
}

void releaseDataRef(const void *d, unsigned long len, void *ptr) {
  struct rtlData *data = (struct rtlData *)ptr;
  --data->references;
  if(data->references == 0) {
    //pthread_rwlock_wrlock(&rtlDataLock);
    //LIST_REMOVE(data, next);
    //pthread_rwlock_unlock(&rtlDataLock);
    dataBlocks--;
    dataBlocksSize -= data->len;
    free(data); // This is a single malloc for both the data and header
  }
}

int sendDataToAllClients(struct rtlData *data) {
  struct client *client;
  pthread_rwlock_rdlock(&clientLock);
  LIST_FOREACH(client, &clients, peer) {
    if(client->flags == CLIENT_READY) {
      struct evbuffer *ev = bufferevent_get_output(client->bev);
      if(evbuffer_get_length(ev) > 4*1024*1024) { // If we've already buffered 4MByte, then start dropping frames
        client->data.dropped += data->len;
        client->data.droppedCount ++;
        continue;
      }
      ++data->references;
      evbuffer_add_reference(ev, data->data, data->len, releaseDataRef, data);
      client->data.out += data->len;
    }
  }
  pthread_rwlock_unlock(&clientLock);
  return data->references;
}

void sendToAllClients(char *buf, size_t len, uint32_t flags) {
  struct client *client;
  pthread_rwlock_rdlock(&clientLock);
  LIST_FOREACH(client, &clients, peer) {
    if((client->flags & flags) != 0)
      bufferevent_write(client->bev, buf, len);
  }
  pthread_rwlock_unlock(&clientLock);
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
  
  slog(level, flag, msg);
}

struct serverInfo {
  enum { SERVER_NEW, SERVER_CONNECTED, SERVER_DISCONNECTED } state;
  char magic[4];
  uint32_t tuner_type;
  uint32_t tuner_gain_count;
  struct {
    unsigned int value;
    unsigned char set;
  } params[0xd]; // Store all the parameters as a simple command array
  struct {
    uint64_t in;
    uint64_t out;
  } data;
} serverInfo;

static void serverErrorEventCB(struct bufferevent *, short, void *);
static void serverReadCB(struct bufferevent *, void *);

static void connectToServer(void *arg) {
  struct bufferevent **serverConnection = (struct bufferevent **)arg;
  slog(LOG_INFO, SLOG_INFO, "Starting connection lookup for %s:%d", config.host, config.port);
  *serverConnection = bufferevent_socket_new(event_base, -1, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_socket_connect_hostname(*serverConnection, NULL, AF_UNSPEC, config.host, config.port);
  slog(LOG_INFO, SLOG_INFO, "Started to connect to %s:%d", config.host, config.port);
  bufferevent_setcb(*serverConnection, serverReadCB, NULL, serverErrorEventCB, serverConnection);
  bufferevent_setwatermark(*serverConnection, EV_READ, 16384, 0);
  bufferevent_enable(*serverConnection, EV_READ|EV_WRITE);
}

static void connectToServerCB(int a, short b, void *arg) {
  connectToServer(arg);
}

static void connectToServerSoon(void *ctx) {
  struct event *ev;
  struct timeval tv;

  tv.tv_sec = 1;
  tv.tv_usec = 0;

  ev = evtimer_new(event_base, connectToServerCB, ctx);
  evtimer_add(ev, &tv);
}

static void serverReadCB(struct bufferevent *bev, void *ctx) {
  struct rtlData *data;
  
  if(serverInfo.state == SERVER_NEW) {
    serverInfo.data.in += bufferevent_read(bev, serverInfo.magic, 4);
    serverInfo.data.in += bufferevent_read(bev, &serverInfo.tuner_type, 4);
    serverInfo.data.in += bufferevent_read(bev, &serverInfo.tuner_gain_count, 4);
    if(serverInfo.magic[0] == 'R' && serverInfo.magic[1] == 'T' && serverInfo.magic[2] == 'L' && serverInfo.magic[3] == '0') {
      serverInfo.state = SERVER_CONNECTED;
      slog(LOG_INFO, SLOG_INFO, "Connected to server.");
    } else { // Failed to receive the magic header
      slog(LOG_ERROR, SLOG_ERROR, "Failed to receive magic header from server.");
      bufferevent_free(bev);
      connectToServerSoon(ctx);
      return;
    }
    // Send stored and set parameters on reconnect
    int i;
    for(i = 0; i < 0xd; i++) {
      if(serverInfo.params[i].set) {
        struct command cmd;
        cmd.cmd = i+1;
        cmd.param = serverInfo.params[i].value;
        slog(LOG_INFO, SLOG_INFO, "Sending command %d with param %lu", cmd.cmd, ntohl(cmd.param));
        serverInfo.data.out += sizeof(cmd);
        bufferevent_write(bev, &cmd, sizeof(cmd));
      }
    }
  }
  
  struct evbuffer *ev = bufferevent_get_input(bev);
  size_t availLen = evbuffer_get_length(ev);
  
  if(availLen == 0) // We may not have data, so return
    return;
  
  if(availLen > 256*1024)
    availLen = 256*1024; // Limit our input sizes to 256k chunks

  data = (struct rtlData *)malloc(sizeof(struct rtlData) + availLen);
  memset(data, 0, sizeof(struct rtlData));
  data->data = (void *)data + sizeof(struct rtlData);
  data->references = 0;
  serverInfo.data.in += data->len = bufferevent_read(bev, data->data, availLen);
  
  if(sendDataToAllClients(data) == 0) {
    // No one was listening
    free(data);
  } else {
    dataBlocks++;
    dataBlocksSize += data->len;    
    // Track the data block
    //pthread_rwlock_wrlock(&rtlDataLock);
    //LIST_INSERT_HEAD(&rtlDataList, data, next);    
    //pthread_rwlock_unlock(&rtlDataLock);
  }
}

static void serverErrorEventCB(struct bufferevent *bev, short events, void *ctx) {
  if (events & BEV_EVENT_ERROR)
    slog(LOG_ERROR, SLOG_ERROR, "Error from server side bufferevent: %s", strerror(errno));
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    bufferevent_free(bev);
    slog(LOG_INFO, SLOG_INFO, "Disconnecting server.");
    serverInfo.state = SERVER_NEW;
    connectToServerSoon(ctx);
  }
}

static void errorEventCB(struct bufferevent *bev, short events, void *ctx) {
  if (events & BEV_EVENT_ERROR)
    slog(LOG_ERROR, SLOG_ERROR, "Error from bufferevent: %s", strerror(errno));
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
  serverInfo.params[cmd.cmd-1].value = cmd.param;
  serverInfo.params[cmd.cmd-1].set = 1;
  slog(LOG_LIVE, SLOG_DEBUG, "Sending command to server: %d: %lu", cmd.cmd, ntohl(cmd.param));
  serverInfo.data.out += sizeof(cmd);
  bufferevent_write(serverConnection, &cmd, sizeof(cmd));
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
  size_t l;
  struct client *client = (struct client *)ctx;
  while((l = bufferevent_read(bev, &cmd, sizeof(cmd))) > 0) {
    client->data.in += l;
    slog(LOG_INFO, SLOG_INFO, "Read from client: %x", cmd.cmd);
    
    switch(cmd.cmd) {
      case RTL_FREQUENCY: // Frequency
      slog(LOG_INFO, SLOG_INFO, "Set frequency: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_SAMPLE_RATE: // Sample rate
      slog(LOG_INFO, SLOG_INFO, "Set sample rate: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN_MODE: // Gain mode
      slog(LOG_INFO, SLOG_INFO, "Set gain mode: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN: // Set Gain
      slog(LOG_INFO, SLOG_INFO, "Set gain: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_FREQ_CORRECTION: // Set freq correction
      slog(LOG_INFO, SLOG_INFO, "Set freq correction: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_STAGE_GAIN: // Stage Gain
      slog(LOG_INFO, SLOG_INFO, "Set stage gain: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_TEST_MODE: // Test mode
      slog(LOG_INFO, SLOG_INFO, "Set test mode: %lu", ntohl(cmd.param));
      break;
      case RTL_AGC_MODE: // AGC mode
      slog(LOG_INFO, SLOG_INFO, "Set AGC mode: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_DIRECT_SAMPLING: // Direct sampling
      slog(LOG_INFO, SLOG_INFO, "Set direct sampling: %lu", ntohl(cmd.param));
      break;
      case RTL_OFFSET_TUNING: // Offset tuning
      slog(LOG_INFO, SLOG_INFO, "Set offset tuning: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_XTAL: // RTL Xtal
      slog(LOG_INFO, SLOG_INFO, "Set RTL xtal: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_TUNER_XTAL: // Tuner Xtal
      slog(LOG_INFO, SLOG_INFO, "Set tuner xtal: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      case RTL_GAIN_BY_INDEX: // Gain by index
      slog(LOG_INFO, SLOG_INFO, "Set gain by index: %lu", ntohl(cmd.param));
      serverSendCommand(cmd);
      break;
      default: // Ignore it
      slog(LOG_INFO, SLOG_INFO, "Ignored client command: %x", cmd.cmd);
    }
  }
}

static void connectCB(struct evconnlistener *listener,
    evutil_socket_t sock, struct sockaddr *addr, int len, void *ptr) {
    struct event_base *base = evconnlistener_get_base(listener);
#ifdef THREADED
    struct bufferevent *bev = bufferevent_socket_new(
            base, sock, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);
#else
    struct bufferevent *bev = bufferevent_socket_new(
            base, sock, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
#endif

    struct client *client = addClient(bev, ptr);
    memcpy(&client->sa, addr, len);
    char ipBuf[128];
    if(client->sa.sa_family == AF_INET)
      evutil_inet_ntop(client->sa.sa_family, &client->sin.sin_addr, ipBuf, 128);
    else if(client->sa.sa_family == AF_INET6)
      evutil_inet_ntop(client->sa.sa_family, &client->sin6.sin6_addr, ipBuf, 128);
    else
      snprintf(ipBuf, 128, "from unknown address");
    slog(LOG_INFO, SLOG_INFO, "Connection from client %s", ipBuf);
    bufferevent_setcb(bev, clientReadCB, NULL, errorEventCB, client);
    bufferevent_enable(bev, EV_READ|EV_WRITE);
    bufferevent_write(bev, serverInfo.magic, 4);
    bufferevent_write(bev, &serverInfo.tuner_type, 4);
    bufferevent_write(bev, &serverInfo.tuner_gain_count, 4);
    serverInfo.data.out += 12;
    client->flags = CLIENT_READY;
}

static void dumpClients(struct evhttp_request *req, void *arg) {
  struct evbuffer *evb = NULL;

  evb = evbuffer_new();

  pthread_rwlock_rdlock(&clientLock);
  
  evbuffer_add_printf(evb, "{\"server\":{\"dataIn\":%lu,\"dataOut\":%lu},\"clients\":[",
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
    evbuffer_add_printf(evb, "{\"client\":{\"host\":\"%s\",\"port\":%u},\"dataIn\":%lu,\"dataOut\":%lu,\"dropped\":{\"size\":%lu,\"count\":%lu},\"connected\":%ld}",
      ipBuf, ntohs(client->sa.sa_family == AF_INET ? client->sin.sin_port : client->sin6.sin6_port),
      client->data.in,
      client->data.out,
      client->data.dropped,
      client->data.droppedCount,
      client->connected
    );
    if(LIST_NEXT(client, peer) != NULL) {
      evbuffer_add_printf(evb, ",");
    }
  }
  evbuffer_add_printf(evb, "]}");
  
  pthread_rwlock_unlock(&clientLock);
  
  evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");
  evhttp_add_header(evhttp_request_get_output_headers(req), "Access-Control-Allow-Origin", "*");
  evhttp_send_reply(req, 200, "OK", evb);
}

void *serverThread(void *arg) {  
  memset(&serverInfo, 0, sizeof(serverInfo));
  
  slog(LOG_INFO, SLOG_INFO, "Starting server thread.");
  
  LIST_INIT(&rtlDataList);
  LIST_INIT(&clients);
  
  pthread_rwlock_init(&rtlDataLock, NULL);
  pthread_rwlock_init(&clientLock, NULL);
  
  event_set_log_callback(logCB);
  
  evthread_use_pthreads();
  
  // Libevent loop
  event_base = event_base_new();
  
  struct sockaddr_in6 sa;
  socklen_t salen = sizeof(sa);
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_addr = in6addr_any;
  sa.sin6_port = htons(config.clientPort);
  
  struct evconnlistener *clientListener;
  clientListener = evconnlistener_new_bind(event_base, connectCB, NULL,
    LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
    (struct sockaddr *)&sa, salen);
  if(!clientListener) {
    timeToExit = 1;
    slog(LOG_FATAL, SLOG_FATAL, "Could not listen on the client streaming port.");
    return NULL;
  }
  
  slog(LOG_INFO, SLOG_INFO, "Listening for clients on port %d", config.clientPort);
  
  connectToServer(&serverConnection);

  struct evhttp *http;
  struct evhttp_bound_socket *handle;
  http = evhttp_new(event_base);

  evhttp_set_cb(http, "/stats.json", dumpClients, "clients");
  
  handle = evhttp_bind_socket_with_handle(http, "::", config.clientPort + 1);

  if(!handle) {
    slog(LOG_FATAL, SLOG_FATAL, "Could not bind HTTP listener.");
    timeToExit = 1;
    return NULL;
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
/*      pthread_rwlock_rdlock(&clientLock);
      struct client *client;
      unsigned long clientCount = 0;
      LIST_FOREACH(client, &clients, peer) {
        clientCount++;
      }
      slog(LOG_INFO, SLOG_INFO, "Clients currently connected: %lu", clientCount);
      pthread_rwlock_unlock(&clientLock);*/
      pthread_rwlock_rdlock(&rtlDataLock);
      if(dataBlocks > 0)
        slog(LOG_INFO, SLOG_INFO, "Maintaining %lu data buffers, total of %lu bytes.", dataBlocks, dataBlocksSize);
      pthread_rwlock_unlock(&rtlDataLock);
    }
  }
  
  pthread_rwlock_wrlock(&clientLock);
  while(LIST_FIRST(&clients) != NULL) {
    struct client *client = LIST_FIRST(&clients);
    LIST_REMOVE(client, peer);
    bufferevent_free(client->bev);
    free(client);
  }
  pthread_rwlock_unlock(&clientLock);
  
  evconnlistener_free(clientListener);
  evhttp_free(http);
  
  event_base_free(event_base);
  
  return NULL;
}
