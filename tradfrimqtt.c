
/* tradfrimqtt
 * 
 * native-c implementation of mqtt to tradfri (CoAP) gateway for control of
 * lightbulbs and outlets. This uses the libcoap library for CoAP/DTLS.
 * 
 * by Paul Tupper 08/2019
 * 
 * This is derived from coap-client which is part of libcoap.
 * 
 * Libcoap:
 * 
 * Copyright (C) 2010--2015 Olaf Bergmann <bergmann@tzi.org>
 * 
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <json-c/json.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#include <coap/debug.h>
#include <coap/mem.h>
#include "coap/coap.h"
#include "coap/coap_dtls.h"
#include "coap/utlist.h"

#include <mosquitto.h>

/* Defaults */

#define APP_VERSION "0.97"

#define DEFAULT_MQTT_START_DELAY 5 /* 5 second delay to allow broker to start */

#define GATEWAY_POLLTIME_S 10   /* Poll device and group parameters every 10 seconds */
#define GATEWAY_SCANTIME_S 900  /* Re-scan for devices/groups every 15 minutes */

#define GATEWAY_SCAN_POLLS (GATEWAY_SCANTIME_S / GATEWAY_POLLTIME_S) /* Poll counts per scan */

#define DEFAULT_TRADFRI_BASE_TOPIC       "tradfri/"   /* Default mqtt topic prefix if not overridden by arg */
#define TRADFRI_STATUS_TOPIC             "status/"     /* Status topic section for mqtt publish e.g. (tradfri/status/<device>....) */
#define TRADFRI_SET_TOPIC                "set/"        /* Set topic section for mqtt subscribe e.g. (tradfri/set/<device>....) */
#define TRADFRI_GET_TOPIC                "get/"        /* Get topic for mqtt subscribe */

/* MQTT Globals */

static sem_t poll_sem;

struct mosquitto *m = NULL;

struct client_info {
    struct mosquitto *m;
    uint32_t tick_ct;
    struct publog *loglist;
};

static struct client_info info;
static pthread_t mosq_thread;

static struct mosquitto *mqtt_init(struct client_info *info, const char *clientid, const char *mqtturl, int port);

/* MQTT Topics */
/* Use 'char x[]=' here instead of 'char *x=' so we can use sizeof() */
const char defaultbasetopic[]    = DEFAULT_TRADFRI_BASE_TOPIC;
const char statussubtopic[]      = TRADFRI_STATUS_TOPIC;
const char setsubtopic[]         = TRADFRI_SET_TOPIC;
const char getsubtopic[]         = TRADFRI_GET_TOPIC;

char *basetopic = (char *)defaultbasetopic; /* Start with the default base topic (this can be changed by cmdline args) */

/* System Logging */
char *logfile = NULL;
FILE* logfd = NULL;
bool logfifo = false;
int delaystart = DEFAULT_MQTT_START_DELAY;

#define LOG_NONE   0
#define LOG_ALWAYS 0
#define LOG_CHANGE 1
#define LOG_ERR    2
#define LOG_TRC    3
#define LOG_DBG    4

int loglevel = LOG_NONE;

static void setup_log(void){  
    if(logfifo == true){
        printf("Opening fifo %s\n", logfile);
        mkfifo(logfile, 0666);
        logfd = fopen(logfile, "w");
    }
}

static void tradfrilog(int level, bool timestamp, const char *format, ...){
    FILE* myfd;
    if(logfile == NULL)
        return;
    if(level > loglevel)
        return;
    
    if(logfd == NULL){
        myfd = fopen(logfile, "a");
    }else{
        myfd = logfd;
    }
    
    if(myfd != NULL){
        if(timestamp){
            time_t t = time(NULL);
            struct tm tm = *localtime(&t);
            fprintf(myfd, "%d/%d %02d:%02d:%02d| ", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min, tm.tm_sec);
        }
    
        va_list arg;
        va_start (arg, format);
        vfprintf(myfd, format, arg);
        va_end (arg);
        if(logfd == NULL && myfd != NULL)
            fclose(myfd);
        if(logfd != NULL)
            fflush(logfd);
    }
}

#define TRADFRILOG(l, x...)     tradfrilog(l, true, x)
#define TRADFRILOGEX(l, x...)   tradfrilog(l, false, x)


/* Options list control */
typedef struct coap_list_t {
  struct coap_list_t *next;
  char data[];
} coap_list_t;

#define MAX_USER 128 /* Maximum length of a user name (i.e., PSK identity) in bytes. */
#define MAX_KEY   64 /* Maximum length of a key (i.e., PSK) in bytes. */

int flags = 0;

/* Use CoAP tokens to keep track of requests/responses */
static unsigned char _token_data[8];
str the_token = { 0, _token_data };

#define FLAGS_BLOCK 0x01

static coap_list_t *optlist = NULL;

/* transfer/transaction is complete when this flag is set */
static int ready = 0;

static str payload = { 0, NULL };       /* optional payload to send */

typedef unsigned char method_t;
method_t method = 1;                    /* the method we are using in our requests */

coap_block_t block = { .num = 0, .m = 0, .szx = 6 };

unsigned int wait_seconds = 5;          /* default timeout in seconds */
coap_tick_t max_wait;                   /* global timeout (changed by set_timeout()) */
unsigned int numfails;

unsigned int obs_seconds = 30;          /* default observe time */
coap_tick_t obs_wait = 0;               /* timeout for current subscription */
int observe = 0;                        /* set to 1 if resource is being observed */

#define min(a,b) ((a) < (b) ? (a) : (b))

#ifdef __GNUC__
#define UNUSED_PARAM __attribute__ ((unused))
#else /* not a GCC */
#define UNUSED_PARAM
#endif /* GCC */

int coap_insert(coap_list_t **head, coap_list_t *node) {
  if (!node) {
    TRADFRILOG(LOG_ERR, "cannot create option node\n");
  } else {
    /* must append at the list end to avoid re-ordering of
     * options during sort */
    LL_APPEND((*head), node);
  }

  return node != NULL;
}

int coap_delete(coap_list_t *node) {
  if (node) {
    coap_free(node);
  }
  return 1;
}

void coap_delete_list(coap_list_t *queue) {
  coap_list_t *elt, *tmp;

  if (!queue)
    return;

  LL_FOREACH_SAFE(queue, elt, tmp) {
    coap_delete(elt);
  }
}

static inline void set_timeout(coap_tick_t *timer, const unsigned int seconds) {
  *timer = seconds * 1000;
}

/* Buffer response in a malloc'd array starting at 500 bytes but grow-able */
#define DEFAULT_OUTBUFLEN 500
static char *outputbuf = NULL;
static int outputbuffil = 0;
static int outputbuflen = DEFAULT_OUTBUFLEN;

static int append_to_output(const unsigned char *data, size_t len) {
    int rc = -1;

    if(outputbuf == NULL){
        outputbuf = malloc(outputbuflen);
    }
    if(outputbuffil + len > outputbuflen){
        outputbuflen = outputbuffil + len + 1;
        outputbuf = realloc(outputbuf, outputbuflen);
    }
    if(outputbuf != NULL){
        snprintf(&outputbuf[outputbuffil], len + 1, "%s", (char *)data);
        outputbuffil += len;
        rc = 0;
    }
    return rc;
}

static int order_opts(void *a, void *b) {
  coap_option *o1, *o2;

  if (!a || !b)
    return a < b ? -1 : 1;

  o1 = (coap_option *)(((coap_list_t *)a)->data);
  o2 = (coap_option *)(((coap_list_t *)b)->data);

  return (COAP_OPTION_KEY(*o1) < COAP_OPTION_KEY(*o2)) ? -1 : (COAP_OPTION_KEY(*o1) != COAP_OPTION_KEY(*o2));
}

/* Include a token in each request that increments to filter out any delayed responses */
static unsigned short seqno = 0;
static void token_inc(void){
    snprintf((char *)the_token.s, sizeof(_token_data), "t%05x", seqno++);
    the_token.length = min(strlen((char *)the_token.s), sizeof(_token_data));
}

static coap_pdu_t *coap_new_request(coap_context_t *ctx, method_t m, coap_list_t **options, unsigned char *data, size_t length) {
  coap_pdu_t *pdu;
  coap_list_t *opt;

  if ( ! ( pdu = coap_new_pdu() ) )
    return NULL;

  pdu->hdr->type = COAP_MESSAGE_CON;
  pdu->hdr->id = coap_new_message_id(ctx);
  pdu->hdr->code = m;

  pdu->hdr->token_length = the_token.length;
  if ( !coap_add_token(pdu, the_token.length, the_token.s)) {
    debug("cannot add token to request\n");
  }

  coap_show_pdu(pdu);

  if (options) {
    /* sort options for delta encoding */
    LL_SORT((*options), order_opts);

    LL_FOREACH((*options), opt) {
      coap_option *o = (coap_option *)(opt->data);
      coap_add_option(pdu, COAP_OPTION_KEY(*o), COAP_OPTION_LENGTH(*o), COAP_OPTION_DATA(*o));
    }
  }

  if (length) {
    if ((flags & FLAGS_BLOCK) == 0)
      coap_add_data(pdu, length, data);
    else
      coap_add_block(pdu, length, data, block.num, block.szx);
  }

  return pdu;
}

static int resolve_host_address(char *server, struct sockaddr *dst) {
    struct addrinfo *res, *ainfo;
    struct addrinfo hints;
    int error, len=-1;

    memset ((char *)&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;

    error = getaddrinfo(server, NULL, &hints, &res);

    if (error != 0) {
        TRADFRILOG(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(error));
        return error;
    }

    for (ainfo = res; ainfo != NULL; ainfo = ainfo->ai_next) {
        switch (ainfo->ai_family) {
            case AF_INET6:
            case AF_INET:
                len = ainfo->ai_addrlen;
                memcpy(dst, ainfo->ai_addr, len);
                goto finish;
            default:
                ;
        }
    }

finish:
    freeaddrinfo(res);
    return len;
}

static inline int check_token(coap_pdu_t *received) {
  return received->hdr->token_length == the_token.length &&
    memcmp(received->hdr->token, the_token.s, the_token.length) == 0;
}

static void message_handler(struct coap_context_t *ctx, const coap_endpoint_t *local_interface, const coap_address_t *remote, coap_pdu_t *sent, coap_pdu_t *received, const coap_tid_t id UNUSED_PARAM) {
  coap_pdu_t *pdu = NULL;
  coap_opt_t *block_opt;
  coap_opt_iterator_t opt_iter;
  unsigned char buf[4];
  coap_list_t *option;
  size_t len;
  unsigned char *databuf;
  coap_tid_t tid;
  
#ifndef NDEBUG
  if (LOG_DEBUG <= coap_get_log_level()) {
    debug("** process incoming %d.%02d response:\n",
          (received->hdr->code >> 5), received->hdr->code & 0x1F);
    coap_show_pdu(received);
  }
#endif

  /* check if this is a response to our original request */
  if (!check_token(received)) {
    /* drop if this was just some message, or send RST in case of notification */
    if (!sent && (received->hdr->type == COAP_MESSAGE_CON || received->hdr->type == COAP_MESSAGE_NON))
      coap_send_rst(ctx, local_interface, remote, received);
    else
        TRADFRILOG(LOG_ERR, "COAP received response for %s but waiting for %s - discarding\n", received->hdr->token, the_token.s);
    return;
  }

  if (received->hdr->type == COAP_MESSAGE_RST) {
    info("got RST\n");
    return;
  }

  /* output the received data, if any */
  if (COAP_RESPONSE_CLASS(received->hdr->code) == 2) {
    /* set obs timer if we have successfully subscribed a resource */
    //if (sent && coap_check_option(received, COAP_OPTION_SUBSCRIPTION, &opt_iter)) {
    //  debug("observation relationship established, set timeout to %d\n", obs_seconds);
    //  set_timeout(&obs_wait, obs_seconds);
    //  observe = 1;
    //}

    /* Got some data, check if block option is set. Behavior is undefined if
     * both, Block1 and Block2 are present. */
    block_opt = coap_check_option(received, COAP_OPTION_BLOCK2, &opt_iter);
    if (block_opt) { /* handle Block2 */
      unsigned short blktype = opt_iter.type;

      /* TODO: check if we are looking at the correct block number */
      if (coap_get_data(received, &len, &databuf))
        append_to_output(databuf, len);

      if(COAP_OPT_BLOCK_MORE(block_opt)) {
        /* more bit is set */
        debug("found the M bit, block size is %u, block nr. %u\n", COAP_OPT_BLOCK_SZX(block_opt), coap_opt_block_num(block_opt));

        /* create pdu with request for next block */
        pdu = coap_new_request(ctx, method, NULL, NULL, 0); /* first, create bare PDU w/o any option  */
        if ( pdu ) {
          /* add URI components from optlist */
          for (option = optlist; option; option = option->next ) {
            coap_option *o = (coap_option *)(option->data);
            switch (COAP_OPTION_KEY(*o)) {
              case COAP_OPTION_URI_HOST :
              case COAP_OPTION_URI_PORT :
              case COAP_OPTION_URI_PATH :
              case COAP_OPTION_URI_QUERY :
                coap_add_option (pdu, COAP_OPTION_KEY(*o), COAP_OPTION_LENGTH(*o), COAP_OPTION_DATA(*o));
                break;
              default:
                ;     /* skip other options */
            }
          }

          /* finally add updated block option from response, clear M bit */
          /* blocknr = (blocknr & 0xfffffff7) + 0x10; */
          debug("query block %d\n", (coap_opt_block_num(block_opt) + 1));
          coap_add_option(pdu, blktype, coap_encode_var_bytes(buf, ((coap_opt_block_num(block_opt) + 1) << 4) | COAP_OPT_BLOCK_SZX(block_opt)), buf);

          if (pdu->hdr->type == COAP_MESSAGE_CON)
            tid = coap_send_confirmed(ctx, local_interface, remote, pdu);
          else
            tid = coap_send(ctx, local_interface, remote, pdu);

          if (tid == COAP_INVALID_TID) {
            debug("message_handler: error sending new request");
            coap_delete_pdu(pdu);
          } else {
            set_timeout(&max_wait, wait_seconds);
            if (pdu->hdr->type != COAP_MESSAGE_CON)
              coap_delete_pdu(pdu);
          }

          return;
        }
      }
    } else { /* no Block2 option */
      block_opt = coap_check_option(received, COAP_OPTION_BLOCK1, &opt_iter);

      if (block_opt) { /* handle Block1 */
        block.szx = COAP_OPT_BLOCK_SZX(block_opt);
        block.num = coap_opt_block_num(block_opt);

        debug("found Block1, block size is %u, block nr. %u\n",
        block.szx, block.num);

        if (payload.length <= (block.num+1) * (1 << (block.szx + 4))) {
          debug("upload ready\n");
          ready = 1;
          return;
        }

        /* create pdu with request for next block */
        pdu = coap_new_request(ctx, method, NULL, NULL, 0); /* first, create bare PDU w/o any option  */
        if (pdu) {

          /* add URI components from optlist */
          for (option = optlist; option; option = option->next ) {
            coap_option *o = (coap_option *)(option->data);
            switch (COAP_OPTION_KEY(*o)) {
              case COAP_OPTION_URI_HOST :
              case COAP_OPTION_URI_PORT :
              case COAP_OPTION_URI_PATH :
              case COAP_OPTION_CONTENT_FORMAT :
              case COAP_OPTION_URI_QUERY :
                coap_add_option (pdu, COAP_OPTION_KEY(*o), COAP_OPTION_LENGTH(*o), COAP_OPTION_DATA(*o));
                break;
              default:
              ;     /* skip other options */
            }
          }

          /* finally add updated block option from response, clear M bit */
          /* blocknr = (blocknr & 0xfffffff7) + 0x10; */
          block.num++;
          block.m = ((block.num+1) * (1 << (block.szx + 4)) < payload.length);

          debug("send block %d\n", block.num);
          coap_add_option(pdu, COAP_OPTION_BLOCK1, coap_encode_var_bytes(buf, (block.num << 4) | (block.m << 3) | block.szx), buf);

          coap_add_block(pdu, payload.length, payload.s, block.num, block.szx);
          coap_show_pdu(pdu);
          if (pdu->hdr->type == COAP_MESSAGE_CON)
            tid = coap_send_confirmed(ctx, local_interface, remote, pdu);
          else
            tid = coap_send(ctx, local_interface, remote, pdu);

          if (tid == COAP_INVALID_TID) {
            debug("message_handler: error sending new request");
            coap_delete_pdu(pdu);
          } else {
            set_timeout(&max_wait, wait_seconds);
            if (pdu->hdr->type != COAP_MESSAGE_CON)
              coap_delete_pdu(pdu);
          }

          return;
        }
      } else {
        /* There is no block option set, just read the data and we are done. */
        if (coap_get_data(received, &len, &databuf))
        append_to_output(databuf, len);
      }
    }
  } else {      /* no 2.05 */

    /* check if an error was signaled and output payload if so */
    if (COAP_RESPONSE_CLASS(received->hdr->code) >= 4) {
      fprintf(stderr, "%d.%02d", (received->hdr->code >> 5), received->hdr->code & 0x1F);
      if (coap_get_data(received, &len, &databuf)) {
        fprintf(stderr, " ");
        while(len--)
        fprintf(stderr, "%c", *databuf++);
      }
      fprintf(stderr, "\n");
    }

  }

  /* finally send new request, if needed */
  if (pdu && coap_send(ctx, local_interface, remote, pdu) == COAP_INVALID_TID) {
    debug("message_handler: error sending response");
  }
  coap_delete_pdu(pdu);

  /* our job is done, we can exit at any time */
  ready = coap_check_option(received, COAP_OPTION_SUBSCRIPTION, &opt_iter) == NULL;
}

static coap_list_t *new_option_node(unsigned short key, unsigned int length, unsigned char *data) {
  coap_list_t *node;

  node = coap_malloc(sizeof(coap_list_t) + sizeof(coap_option) + length);

  if (node) {
    coap_option *option;
    option = (coap_option *)(node->data);
    COAP_OPTION_KEY(*option) = key;
    COAP_OPTION_LENGTH(*option) = length;
    memcpy(COAP_OPTION_DATA(*option), data, length);
  } else {
    TRADFRILOG(LOG_ERR, "new_option_node: malloc failed\n");
  }

  return node;
}

typedef struct {
  unsigned char code;
  char *media_type;
} content_type_t;

static coap_context_t *get_context(const char *node, const char *port, int secure) {
  coap_context_t *ctx = NULL;
  int s;
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int ep_type;

  ctx = coap_new_context(NULL);
  if (!ctx) {
    return NULL;
  }

  ep_type = secure ? COAP_ENDPOINT_DTLS : COAP_ENDPOINT_NOSEC;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_DGRAM; /* Coap uses UDP */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV | AI_ALL;

  s = getaddrinfo(node, port, &hints, &result);
  if ( s != 0 ) {
    TRADFRILOG(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(s));
    return NULL;
  }

  /* iterate through results until success */
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    coap_address_t addr;
    coap_endpoint_t *endpoint;

    if (rp->ai_addrlen <= sizeof(addr.addr)) {
      coap_address_init(&addr);
      addr.size = rp->ai_addrlen;
      memcpy(&addr.addr, rp->ai_addr, rp->ai_addrlen);

      endpoint = coap_new_endpoint(&addr, ep_type);
      if (endpoint) {
        coap_attach_endpoint(ctx, endpoint);
        goto finish;
      } else {
        TRADFRILOG(LOG_ERR, "cannot create coap endpoint\n");
        continue;
      }
    }
  }

  TRADFRILOG(LOG_ERR, "no context available for interface '%s'\n", node);
  coap_free_context(ctx);
  ctx = NULL;

 finish:
  freeaddrinfo(result);
  return ctx;
}

coap_context_t  *ctx = NULL;
coap_address_t dst;
  
char *gateway_url = NULL;
int gateway_port = 5684;
char *user = NULL;
int userlen = 0;
char *key = NULL;
int keylen = 0;

coap_log_t log_level = LOG_WARNING;

int coap_execute(method_t m, const char *path, char *payload, char **resp){
    coap_pdu_t  *pdu;
    coap_tid_t tid = COAP_INVALID_TID;
    int result = -1;
    coap_tick_t start, now;
    int payloadlen = 0;
    
    char *editpath = strdup(path);
    char *pathelem = editpath;
    char *pathwalk = editpath;
    
    bool failed = false;
    
    /* Make 'unique' token for this request */
    token_inc();
    method = m;
    
    *resp = NULL;
    /* Zero the output buffer index */
    outputbuffil = 0;
    outputbuflen = DEFAULT_OUTBUFLEN;
    /* Will be allocated when we try to buffer a response */
    outputbuf = NULL;
    
    while(pathelem != NULL){
        while(*pathwalk != 0 && *pathwalk != '/')
            pathwalk++;
        if(*pathwalk == '/')
            *pathwalk++ = 0;
        coap_insert(&optlist, new_option_node(COAP_OPTION_URI_PATH, (unsigned int)strlen(pathelem), (unsigned char *)pathelem));
        if(*pathwalk != 0)
            pathelem = pathwalk;
        else
            pathelem = NULL;
    }
    free(editpath);
    
    if(payload)
        payloadlen = strlen(payload);
    
    if (! (pdu = coap_new_request(ctx, m, &optlist, (unsigned char *)payload, payloadlen))) {
        return -1;
    }
    
    tid = coap_send_confirmed(ctx, ctx->endpoint, &dst, pdu);
    
    if (tid == COAP_INVALID_TID)
        coap_delete_pdu(pdu);
    
    set_timeout(&max_wait, wait_seconds);
    TRADFRILOG(LOG_DBG, "timeout is set to %d seconds\n", wait_seconds);

    coap_ticks(&start);
    while (!(ready && coap_can_exit(ctx))) {
        unsigned int wait_ms = max_wait;
        result = coap_run_once(ctx, wait_ms);
        if (result >= 0) {
            coap_ticks(&now);
            if (start + wait_seconds * COAP_TICKS_PER_SECOND < now) {
                ready = 1;
                failed = true;
            }
            if ((unsigned int)result < max_wait) {
                max_wait -= result;
            }
        }
    }

    coap_delete_list(optlist);
    optlist = NULL;
    
    /* Return the output buffer */
    if(outputbuf != NULL){
        outputbuf[outputbuffil] = 0;
        *resp = outputbuf;
    }
    
    /* Return -1 if we timed out */
    return failed == true ? -1 : 0;  
}

#define SET_BULB_DIMMER   "{ \"3311\": [{ \"5851\": %d, \"5712\": 10 }] }"
#define SET_BULB_POWER    "{ \"3311\": [{ \"5850\": %d }] }"
#define SET_BULB_TEMP     "{ \"3311\": [{ \"5709\" : %d, \"5710\": %d, \"5712\": 10 }] }"
//#define SET_TEMP   "{ \"3311\" : [{ \"5711\": %d, \"5712\": 10 }] }"
#define CONVERTTEMP_PC_TO_X(pc)    (24930 + (int)(82.05 * (float)pc))               /* 24930 - 33135 */  /* 0x6162 - 0x816f */
#define CONVERTTEMP_PC_TO_Y(pc)    (24694 + (int)(25.17 * (float)(100 - pc)))       /* 24694 - 27211 */ /* 0x6076 - 0x6a4b */
#define CONVERTTEMP_X_TO_PC(x)     ((int)((float)(x - 24930) / 82.05))

#define SET_OUTLET_POWER  "{ \"3312\": [{ \"5850\": %d }] }"

#define SET_BLIND_HEIGHT  "{ \"15015\": [{ \"5536\": %.1f }] }"
#define SET_BLIND_TRIGGER "{ \"15015\": [{ \"5523\": %.1f }] }"

#define SET_GROUP_DIMMER  "{ \"5851\": %d, \"5712\": 10 }"
#define SET_GROUP_POWER   "{ \"5850\": %d }"

/* Add new device types here */
enum _devicetype{DEVICE_TYPE_UNKNOWN = 0, DEVICE_TYPE_BULB, DEVICE_TYPE_DIMMER, DEVICE_TYPE_OUTLET, DEVICE_TYPE_BLIND};

/* Command types for devices */
enum _tradfri_cmd {COMMAND_NONE = 0, COMMAND_POWER, COMMAND_BRIGHTNESS, COMMAND_COLOURTEMP, COMMAND_RGB, COMMAND_HEIGHT};

/* Device structure - one entry per device */
struct device {
    int id;
    char *name;
    bool report;
    bool removed;
    bool batterypowered;
    int batterylevel;
    bool batterylow;
    bool reportbatterylevel;
    enum _devicetype type; 
    void *typedata;
    struct device *next;
};

#define NOCHANGE       -1
#define NOCHANGE_FLOAT -1.0

/* Bulb type structure - one per bulb device */
struct bulb {
    bool iscolourtemp;
    bool isrgb;
    bool mainson;
    bool reportmainson;
    bool poweron;
    bool reportpower;
    int brightness;
    bool reportbrightness;
    int colour;
    bool reportcolour;
    int desiredpower;
    int desiredbrightness;
    int desiredcolour;
    struct device *parent;
};

/* Outlet (remote mains plug/socket) struct */
struct outlet {
    bool mainson;
    bool reportmainson;
    bool poweron;
    int desiredpower;
    bool reportpower;
    struct device *parent;
};

/* Blind struct */
struct blind {
    float height;
    float desiredheight;
    bool reportheight;
    struct device *parent;
};

/* List sub-structure (list) for devices in a group */
struct grp_devlist {
    int numdevs;
    /* Make sure this is at the end */
    struct device *dev[0];
};

/* Group structure to contain devices within a tradfri group */
struct group {
    int id;
    char *name;
    bool poweron;
    int desiredpower;
    int brightness;
    int desiredbrightness;
    bool removed;
    struct group *next;
    /* Make sure this is at the end */
    struct grp_devlist *devices;
};

/* List head for devices */
struct device *devicehead = NULL;

/* List head for groups */
struct group *grouphead = NULL;

/* Create a new device struct and add it to the devlist */
static int newdevice(int id){
    int rc = -1;
    struct device *newdevice = malloc(sizeof(struct device));
    if(newdevice != NULL){
        newdevice->id = id;
        newdevice->name = NULL;
        newdevice->type = DEVICE_TYPE_UNKNOWN;
        newdevice->typedata = NULL;
        newdevice->report = false;
        newdevice->removed = false;
        newdevice->batterypowered = false;
        newdevice->batterylevel = 0;
        newdevice->batterylow = false;
        newdevice->reportbatterylevel = false;
        newdevice->next = NULL;
        if(devicehead == NULL){
            devicehead = newdevice;
        }else{
            struct device *devicewalk = devicehead;
            while(devicewalk->next != NULL)
                devicewalk = devicewalk->next;
            devicewalk->next = newdevice;
        }
        rc = 0;
    }
    return rc;
}

static int deletedevice(struct device *dev){
    int rc = -1;
    struct device *prevdev = NULL, *thisdev = devicehead;
    while(thisdev != NULL){
        if(thisdev == dev){
            if(prevdev == NULL)
                devicehead = dev->next;
            else
                prevdev->next = dev->next;
            if(dev->name != NULL)
                free(dev->name);
            if(dev->typedata != NULL)
                free(dev->typedata);
            free(dev);
            rc = 0;
            break;
        }
        prevdev = thisdev;
        thisdev = thisdev->next;
    }
    return rc;
}

/* Create a new group struct and add it to the grouplist */
static int newgroup(int id){
    int rc = -1;
    struct group *newgroup = malloc(sizeof(struct group));
    if(newgroup != NULL){
        newgroup->id = id;
        newgroup->name = NULL;
        newgroup->poweron = false;
        newgroup->desiredpower = NOCHANGE;
        newgroup->brightness = 0;
        newgroup->desiredbrightness = NOCHANGE;
        newgroup->devices = NULL;
        newgroup->removed = false;
        newgroup->next = NULL;
        if(grouphead == NULL){
            grouphead = newgroup;
        }else{
            struct group *groupwalk = grouphead;
            while(groupwalk->next != NULL)
                groupwalk = groupwalk->next;
            groupwalk->next = newgroup;
        }
        rc = 0;
    }
    return rc;
}

static int deletegroup(struct group *grp){
    int rc = -1;
    struct group *prevgrp = NULL, *thisgrp = grouphead;
    while(thisgrp != NULL){
        if(thisgrp == grp){
            if(prevgrp == NULL)
                grouphead = grp->next;
            else
                prevgrp->next = grp->next;
            if(grp->name != NULL)
                free(grp->name);
            if(grp->devices != NULL)
                free(grp->devices);
            free(grp);
            rc = 0;
            break;
        }
        prevgrp = thisgrp;
        thisgrp = thisgrp->next;
    }
    return rc;
}

/* Device type descriptors from element '6' of the type parameters reported by the tradfri gateway */
//static enum _devicetype  param6type[6] = {DEVICE_TYPE_UNKNOWN, DEVICE_TYPE_BULB, DEVICE_TYPE_UNKNOWN, DEVICE_TYPE_DIMMER, DEVICE_TYPE_UNKNOWN, DEVICE_TYPE_OUTLET};
/* Text descriptions of the above */
//static const char      *param6descr[6] = {"unknown", "Bulb", "unknown", "Dimmer", "unknown", "Outlet"};

/* Query device details from the gateway */
static int update_device(struct device *thisdevice){
    char *output = NULL;
    int res, reportedid = 0;
    int batterylevel = -1;
    char path[200];
    
    sprintf(path, "15001/%d", thisdevice->id);
    TRADFRILOG(LOG_DBG, "Get device details for %s\n", path);
    res = coap_execute(COAP_REQUEST_GET, path, NULL, &output);
    if(res == 0 && output != NULL){
        TRADFRILOG(LOG_DBG, "Device %d details %s\n", thisdevice->id, output);
        const char *name;
        json_object *jobj = json_tokener_parse(output);
            
        json_object *jparse, *jsubparse, *jarrparse; 
        
        /* Sometimes the responses seem to get queued in the gateway - discard non-matching ids */
        if(json_object_object_get_ex(jobj, "9003", &jparse)){
            reportedid = json_object_get_int(jparse);
        }
        /* If the ids match we've got the right device */
        if(reportedid == thisdevice->id){
            /* 9001 is the name */
            if(json_object_object_get_ex(jobj, "9001", &jparse)){
                name = json_object_get_string(jparse);
                if(thisdevice->name == NULL){
                    TRADFRILOG(LOG_CHANGE, "Device name %s\n", name);
                    thisdevice->name = strdup(name);
                    /* 3 is the device details */
                    if(json_object_object_get_ex(jobj, "3", &jparse)){
                        /* Read the device description and display it */
                        if(json_object_object_get_ex(jparse, "1", &jsubparse)){
                            name = json_object_get_string(jsubparse);
                            TRADFRILOG(LOG_CHANGE, "Device %s is a %s\n", thisdevice->name, name);
                        }
                        /* "6" is power supply (1=mains, 3=battery) */
                        if(json_object_object_get_ex(jparse, "6", &jsubparse)){
                            if(3 == json_object_get_int(jsubparse)){
                                TRADFRILOG(LOG_TRC, "Device %s is battery powered\n", thisdevice->name);
                                thisdevice->batterypowered = true;
                            }
                        }
                    }
                    thisdevice->report = true;
                }else if(strcmp(name, thisdevice->name) != 0){
                    TRADFRILOG(LOG_CHANGE, "Device %s(%d) changed its name to %s\n", thisdevice->name, thisdevice->id, name);
                    free(thisdevice->name);
                    thisdevice->name = strdup(name);
                    thisdevice->report = true;
                }
            }
            
            /* For battery powered devices check the battery level */
            if(thisdevice->batterypowered == true){
                if(json_object_object_get_ex(jobj, "3", &jparse)){
                    if(json_object_object_get_ex(jparse, "9", &jsubparse)){
                        batterylevel = json_object_get_int(jsubparse);
                    }
                }
            }
            
            if(json_object_object_get_ex(jobj, "3312", &jparse)){
                /* 3312 indicates this device is a switchable power outlet */
                bool addingoutlet = false;
                if(thisdevice->typedata == NULL){
                    struct outlet *newoutlet = malloc(sizeof(struct outlet));
                    if(newoutlet != NULL){
                        thisdevice->type = DEVICE_TYPE_OUTLET;
                        addingoutlet = true;
                        newoutlet->parent = thisdevice;
                        newoutlet->mainson = false;
                        newoutlet->reportmainson = false;
                        newoutlet->poweron = false;
                        newoutlet->reportpower = false;
                        newoutlet->desiredpower = NOCHANGE;
                        thisdevice->typedata = newoutlet;
                    }
                }
                struct outlet *thisoutlet = (struct outlet *)(thisdevice->typedata);
                if(thisoutlet != NULL){
                    /* 3312 is an array of settings */
                    //if(json_object_object_get_ex(jobj, "3312", &jparse)){
                        jarrparse = json_object_array_get_idx(jparse, 0);
                        /* 5850 is the power on state */
                        if(json_object_object_get_ex(jarrparse, "5850", &jsubparse)){
                            /* on = 1, off = 0 */
                            int poweron = json_object_get_int(jsubparse);
                            if(thisoutlet->poweron != poweron){
                                TRADFRILOG(LOG_CHANGE, "%s poweronstate changed from %d to %d\n", thisoutlet->parent->name, thisoutlet->poweron, poweron);
                                thisoutlet->poweron = poweron;
                                thisoutlet->reportpower = true;
                            }
                            if(addingoutlet == true)
                                TRADFRILOG(LOG_TRC, "%s poweronstate is %s\n", thisoutlet->parent->name, thisoutlet->poweron ? "on" : "off");
                        }
                    //}
                    /* 9019 is the mains power state for the bulb */
                    if(json_object_object_get_ex(jobj, "9019", &jparse)){
                        int mainson = json_object_get_int(jparse);
                        if(thisoutlet->mainson != mainson){
                            TRADFRILOG(LOG_CHANGE, "%s mainsonstate changed from %d to %d\n", thisoutlet->parent->name, thisoutlet->mainson, mainson);
                            thisoutlet->mainson = mainson;
                            thisoutlet->reportmainson = true;
                        }
                        if(addingoutlet == true)
                            TRADFRILOG(LOG_TRC, "%s mains power is %s\n", thisoutlet->parent->name, thisoutlet->mainson ? "on" : "off");
                    }
                }
            }else if(json_object_object_get_ex(jobj, "15015", &jparse)){
                /* 15015 is a window-blind */
                bool addingblind = false;
                if(thisdevice->typedata == NULL){
                    struct blind *newblind = malloc(sizeof(struct blind));
                    if(newblind != NULL){
                        thisdevice->type = DEVICE_TYPE_BLIND;
                        addingblind = true;
                        newblind->parent = thisdevice;
                        newblind->height = 0.0;
                        newblind->reportheight = false;
                        newblind->desiredheight = NOCHANGE_FLOAT;
                        thisdevice->typedata = newblind;
                    }
                }
                struct blind *thisblind = (struct blind *)(thisdevice->typedata);
                if(thisblind != NULL){
                    jarrparse = json_object_array_get_idx(jparse, 0);
                    /* 5536 is the height */
                    if(json_object_object_get_ex(jarrparse, "5536", &jsubparse)){
                        float height = (float)json_object_get_double(jsubparse);
                        if(thisblind->height != height){
                            TRADFRILOG(LOG_CHANGE, "%s height changed from %.1f to %.1f\n", thisblind->parent->name, thisblind->height, height);
                            thisblind->height = height;
                            thisblind->reportheight = true;
                        }
                        if(addingblind == true)
                            TRADFRILOG(LOG_TRC, "%s height is %.1f\n", thisblind->parent->name, thisblind->height);
                    }
                }
            }else if(json_object_object_get_ex(jobj, "3311", &jparse)){
                /* 3311 is a lightbulb */
                bool addingbulb = false;
                if(thisdevice->typedata == NULL){
                    struct bulb *newbulb = malloc(sizeof(struct bulb));
                    if(newbulb != NULL){
                        thisdevice->type = DEVICE_TYPE_BULB;
                        addingbulb = true;
                        newbulb->parent = thisdevice;
                        thisdevice->typedata = newbulb;
                        newbulb->iscolourtemp = false;
                        newbulb->isrgb = false;
                        newbulb->mainson = false;
                        newbulb->reportmainson = false;
                        newbulb->poweron = false;
                        newbulb->reportpower = false;
                        newbulb->brightness = 0;
                        newbulb->reportbrightness = false;
                        newbulb->colour = 0;
                        newbulb->reportcolour = false;
                        newbulb->desiredpower = NOCHANGE;
                        newbulb->desiredbrightness = NOCHANGE;
                        newbulb->desiredcolour = NOCHANGE;
                    }
                }
                struct bulb *thisbulb = (struct bulb *)(thisdevice->typedata);
                if(thisbulb != NULL){              
                    jarrparse = json_object_array_get_idx(jparse, 0);
                    /* 5709 is the colour */
                    if(json_object_object_get_ex(jarrparse, "5709", &jsubparse)){
                        thisbulb->iscolourtemp = true;
                        int colour = json_object_get_int(jsubparse);
                        colour = CONVERTTEMP_X_TO_PC(colour);
                        if(thisbulb->colour != colour){
                            TRADFRILOG(LOG_CHANGE, "%s temp changed from %d to %d\n", thisbulb->parent->name, thisbulb->colour, colour);
                            thisbulb->colour = colour;
                            thisbulb->reportcolour = true;
                        }
                        if(addingbulb == true)
                            TRADFRILOG(LOG_TRC, "%s colourtemp %d\n", thisbulb->parent->name, thisbulb->colour);
                    }
                    /* 5850 is the on/off state */
                    if(json_object_object_get_ex(jarrparse, "5850", &jsubparse)){
                        /* on = 1, off = 0 */
                        int poweron = json_object_get_int(jsubparse);
                        if(thisbulb->poweron != poweron){
                            TRADFRILOG(LOG_CHANGE, "%s poweronstate changed from %d to %d\n", thisbulb->parent->name, thisbulb->poweron, poweron);
                            thisbulb->poweron = poweron;
                            thisbulb->reportpower = true;
                        }
                        if(addingbulb == true)
                            TRADFRILOG(LOG_TRC, "%s poweronstate is %s\n", thisbulb->parent->name, thisbulb->poweron ? "on" : "off");
                    }
                    /* 5851 is the brightness (0-254) */
                    if(json_object_object_get_ex(jarrparse, "5851", &jsubparse)){
                        /* Brightness */
                        int brightness = json_object_get_int(jsubparse);
                        if(thisbulb->brightness != brightness){
                            TRADFRILOG(LOG_CHANGE, "%s brightness changed from %d to %d\n", thisbulb->parent->name, thisbulb->brightness, brightness);
                            thisbulb->brightness = brightness;
                            thisbulb->reportbrightness = true;
                        }
                        if(addingbulb == true)
                            TRADFRILOG(LOG_TRC, "%s brightness is %d\n", thisbulb->parent->name, thisbulb->brightness);
                    }
                    /* 9019 is the mains power state for the bulb */
                    if(json_object_object_get_ex(jobj, "9019", &jparse)){
                        int mainson = json_object_get_int(jparse);
                        if(thisbulb->mainson != mainson){
                            TRADFRILOG(LOG_CHANGE, "%s mainsonstate changed from %d to %d\n", thisbulb->parent->name, thisbulb->mainson, mainson);
                            thisbulb->mainson = mainson;
                            thisbulb->reportmainson = true;
                        }
                        if(addingbulb == true)
                            TRADFRILOG(LOG_TRC, "%s mains power is %s\n", thisbulb->parent->name, thisbulb->mainson ? "on" : "off");
                    }
                }
            }else{
                /* Some unrecognised device - we cannot control it but we can monitor its battery level if it is battery powered */
                TRADFRILOG(LOG_DBG, "%s is not a controllable tradfri device\n", thisdevice->name);
            }
            /* If this device is battery powered we can report the battery state even if it contains nothing controllable */
            if(thisdevice->batterypowered == true && batterylevel >= 0){
                int levdiff = abs(thisdevice->batterylevel - batterylevel);
                bool newbattlow = false;
                bool reportbatt = false;
                if(batterylevel <= 10)
                    newbattlow = true;
                else
                    newbattlow = false;
                if(levdiff > 5 || thisdevice->batterylow != newbattlow)
                    reportbatt = true;
                thisdevice->batterylow = newbattlow;
                thisdevice->batterylevel = batterylevel;
                if(reportbatt == true){
                    thisdevice->reportbatterylevel = true;
                    TRADFRILOG(LOG_CHANGE, "%s battery level changed to %d%% - %s\n", thisdevice->name, thisdevice->batterylevel, thisdevice->batterylow == true ? "LOW" : "OK");
                }
            }
        }else{
            TRADFRILOG(LOG_ERR, "Error wrong device (%d) returned when polling %d\n", reportedid, thisdevice->id);
        }
        json_object_put(jobj);
    }else{
        numfails++;
        TRADFRILOG(LOG_ERR, "Error polling %s\n", thisdevice->name);
    }
    if(output != NULL){
        free(output);
    }
    return res;
}

static int command_device(struct device *dev, enum _tradfri_cmd command, char *param){
    int rc = -1;
    int rq = strtol(param, NULL, 10);
    if(dev->type == DEVICE_TYPE_BULB){
        switch(command){
            case COMMAND_POWER:
                /* Check if param is on/off string */
                if(rq == 0 && strcasecmp(param, "ON") == 0)
                    rq = 1;
                TRADFRILOG(LOG_CHANGE, "Set %s power to %s (%d)\n", dev->name, param, rq);
                ((struct bulb *)(dev->typedata))->desiredpower = (rq > 0) ? 1 : 0;
                rc = 0;
                break;
            case COMMAND_BRIGHTNESS:
                TRADFRILOG(LOG_CHANGE, "Set %s brightness to %s\n", dev->name, param);
                float mult = (float)rq * 2.54;
                rq = (int)mult;
                ((struct bulb *)(dev->typedata))->desiredbrightness = rq;
                rc = 0;
                break;
            case COMMAND_COLOURTEMP:
                if(((struct bulb *)(dev->typedata))->iscolourtemp == true){
                    TRADFRILOG(LOG_CHANGE, "Set %s colourtemp to %s\n", dev->name, param);
                    ((struct bulb *)(dev->typedata))->desiredcolour = rq;
                    rc = 0;
                }
                break;
            case COMMAND_RGB:
            default:
                break;
        }
    }else if(dev->type == DEVICE_TYPE_OUTLET){
        int rq = strtol(param, NULL, 10);
        switch(command){
            case COMMAND_POWER:
                /* Check if param is on/off string */
                if(rq == 0 && strcasecmp(param, "ON") == 0)
                    rq = 1;
                TRADFRILOG(LOG_CHANGE, "Set %s power to %s (%d)\n", dev->name, param, rq);
                ((struct outlet *)(dev->typedata))->desiredpower = (rq > 0) ? 1 : 0;
                rc = 0;
                break;
            default:
                break;
        }
    }else if(dev->type == DEVICE_TYPE_BLIND){
        float rq = strtof(param, NULL);
        switch(command){
            case COMMAND_HEIGHT:
                TRADFRILOG(LOG_CHANGE, "Set %s height to %s (%.1f)\n", dev->name, param, rq);
                ((struct blind *)(dev->typedata))->desiredheight = rq;
                rc = 0;
                break;
            default:
                break;
        }
    }
    /* TODO - deal with other devices */
    
    /* Increment the semaphore that wakes the action thread but only if it is currently blocking i.e. its value is zero */
    int semval;
    sem_getvalue(&poll_sem, &semval);
    if(semval == 0)
        sem_post(&poll_sem);
    
    return rc;
}

/* Given a reference string search the device list. First check for numeric ID match
 * If no match compare to device name */
static struct device *finddevice(char *ref){
    struct device *devicewalk = devicehead;
    int id = strtol(ref, NULL, 10);
    if(id != 0){
        /* numeric id so compare to ids */
        while(devicewalk != NULL){
            if(devicewalk->id == id)
                return devicewalk;
            devicewalk = devicewalk->next;
        }
    }
    /* no numeric match so compare ref to name */
    devicewalk = devicehead;
    while(devicewalk != NULL){
        if(strcmp(devicewalk->name, ref) == 0)
            return devicewalk;
        devicewalk = devicewalk->next;
    }
    return NULL;
}

/* Find device by numeric id */
static struct device *finddevid(int devid){
    struct device *devwalk = devicehead;
    while(devwalk != NULL){
        if(devwalk->id == devid)
            return devwalk;
        devwalk = devwalk->next;
    }
    return NULL;
}


/* Flag all devices removed until we reconfirm their existence */
static void flagalldevicesremoved(void){
    struct device *devwalk = devicehead;
    while(devwalk != NULL){
        devwalk->removed = true;
        devwalk = devwalk->next;
    }
}

/* Read the details from the gateway for this group */
static int update_group(struct group *thisgroup){
    char *output = NULL;
    int res, reportedid = 0;
    char path[200];
    
    sprintf(path, "15004/%d", thisgroup->id);
    TRADFRILOG(LOG_DBG, "Get group details for %s\n", path);
    res = coap_execute(COAP_REQUEST_GET, path, NULL, &output);
    if(res == 0 && output != NULL){
        TRADFRILOG(LOG_DBG, "Group %d details %s\n", thisgroup->id, output);
        const char *name;
        json_object *jobj = json_tokener_parse(output);
            
        json_object *jparse, *jarrparse; /*Simply get the obj*/
        
        /* Sometimes the responses seem to get queued in the gateway - discard non-matching ids */
        if(json_object_object_get_ex(jobj, "9003", &jparse)){
            reportedid = json_object_get_int(jparse);
        }
        /* If the ids match we've got the right group */
        if(reportedid == thisgroup->id){
            /* 9001 is the name */
            if(json_object_object_get_ex(jobj, "9001", &jparse)){
                name = json_object_get_string(jparse);
                if(thisgroup->name == NULL){
                    TRADFRILOG(LOG_TRC, "Group name is %s\n", name);
                    thisgroup->name = strdup(name);
                
                    if(json_object_object_get_ex(jobj, "5850", &jparse)){
                        int power = json_object_get_int(jparse);
                        TRADFRILOG(LOG_TRC, "Group power is %s\n", power == 0 ? "off" : "on");
                        thisgroup->poweron = (power == 0 ? false : true);
                    }
                    if(json_object_object_get_ex(jobj, "5851", &jparse)){
                        int bright = json_object_get_int(jparse);
                        TRADFRILOG(LOG_TRC, "Group brightness is %d\n", bright);
                        thisgroup->brightness = bright;
                    }
                    /* Get list of devices and reference them in the group struct */
                    if(json_object_object_get_ex(jobj, "9018", &jparse)){
                        json_object *jparse2;
                        if(json_object_object_get_ex(jparse, "15002", &jparse2)){
                            json_object *jparse3;
                            if(json_object_object_get_ex(jparse2, "9003", &jparse3)){
                                int devidx = 0, numdevs, devid;
                                numdevs = json_object_array_length(jparse3);
                    
                                thisgroup->devices = malloc(sizeof(struct grp_devlist) + (sizeof(struct device *) * numdevs));
                                if(thisgroup->devices != NULL){
                                    TRADFRILOG(LOG_DBG, "This group contains:\n");
                                    thisgroup->devices->numdevs = numdevs;
                                    for(devidx =0; devidx < numdevs; devidx++){
                                        jarrparse = json_object_array_get_idx(jparse3, devidx);
                                        if(jarrparse == NULL)
                                            break;
                                        devid = json_object_get_int(jarrparse);
                                        thisgroup->devices->dev[devidx] = finddevid(devid);
                                        if(thisgroup->devices->dev[devidx] != NULL)
                                            TRADFRILOG(LOG_DBG, "Device %s\n", thisgroup->devices->dev[devidx]->name);
                                    }
                                }
                            }
                        }
                    }
                }else if(strcmp(thisgroup->name, name) != 0){
                    TRADFRILOG(LOG_CHANGE, "Group %s (%d) has changed its name to %s\n", thisgroup->name, thisgroup->id, name);
                    free(thisgroup->name);
                    thisgroup->name = strdup(name);
                }
            }
        }else{
            TRADFRILOG(LOG_ERR, "Error wrong group (%d) returned when polling %d\n", reportedid, thisgroup->id);
        }
        json_object_put(jobj);
    }else{
        numfails++;
        TRADFRILOG(LOG_ERR, "Unable to update group %s\n", thisgroup->name);
    }
    if(output != NULL){
        free(output);
    }
    return res;
}

/* Send a command to all devices in a group */
static int command_group(struct group *grp, enum _tradfri_cmd command, char *param){
    int dev;
    for(dev = 0; dev < grp->devices->numdevs; dev++)
        command_device(grp->devices->dev[dev], command, param);
    return 0;
}

/* Given a reference string search the group list. First check for numeric ID match
 * If no match compare to group name */
static struct group *findgroup(char *ref){
    struct group *groupwalk = grouphead;
    int id = strtol(ref, NULL, 10);
    if(id != 0){
        /* numeric id so compare to ids */
        while(groupwalk != NULL){
            if(groupwalk->id == id)
                return groupwalk;
            groupwalk = groupwalk->next;
        }
    }
    /* no numeric match so compare ref to name */
    groupwalk = grouphead;
    while(groupwalk != NULL){
        if(strcmp(groupwalk->name, ref) == 0)
            return groupwalk;
        groupwalk = groupwalk->next;
    }
    return NULL;
}

/* Find group by numeric id */
static struct group *findgroupid(int groupid){
    struct group *grpwalk = grouphead;
    while(grpwalk != NULL){
        if(grpwalk->id == groupid)
            return grpwalk;
        grpwalk = grpwalk->next;
    }
    return NULL;
}

/* Flag all groups removed until we reconfirm their existence */
static void flagallgroupsremoved(void){
    struct group *grpwalk = grouphead;
    while(grpwalk != NULL){
        grpwalk->removed = true;
        grpwalk = grpwalk->next;
    }
}
    
/* Run through the device list and send any changes */
/* coap_execute errors cause a non-zero return to trigger a context reset */
static int rundeviceupdate(void){
    struct device *thisdev = devicehead;
    char path[100];
    char payload[150];
    char *output = NULL;
    int rc = 0;
    while(thisdev != NULL){
        switch(thisdev->type){
            case DEVICE_TYPE_BULB:
            {
                struct bulb *thisbulb = (struct bulb *)(thisdev->typedata);
                if(thisbulb->desiredpower != NOCHANGE){
                    sprintf(path, "15001/%d", thisdev->id);
                    sprintf(payload, SET_BULB_POWER, thisbulb->desiredpower);
                    if(coap_execute(COAP_REQUEST_PUT, path, payload, &output) == 0){
                        thisbulb->desiredpower = NOCHANGE;
                    }else{
                        rc = 1;
                        numfails++;
                    }
                    if(output != NULL)
                        free(output);
                }
                if(thisbulb->desiredbrightness != NOCHANGE){
                    sprintf(path, "15001/%d", thisdev->id);
                    sprintf(payload, SET_BULB_DIMMER, thisbulb->desiredbrightness);
                    if(coap_execute(COAP_REQUEST_PUT, path, payload, &output) == 0){
                        thisbulb->desiredbrightness = NOCHANGE;
                    }else{
                        rc = 1;
                        numfails++;
                    }
                    if(output != NULL)
                        free(output);
                }
                if(thisbulb->desiredcolour != NOCHANGE){
                    int tempx = CONVERTTEMP_PC_TO_X(thisbulb->desiredcolour);  /* 24930 - 33135 */  /* 0x6162 - 0x816f */
                    int tempy = CONVERTTEMP_PC_TO_Y(thisbulb->desiredcolour); /* 24694 - 27211 */ /* 0x6076 - 0x6a4b */
                    sprintf(path, "15001/%d", thisdev->id);
                    sprintf(payload, SET_BULB_TEMP, tempx, tempy);
                    //sprintf(payload, SET_TEMP, (int)((float)(thisbulb->desiredcolour) * 655.35));
                    //sprintf(payload, SET_TEMP, thisbulb->desiredcolour * 2 + 250);
                    //sprintf(payload, SET_TEMP, ((2200 - 4000) / 100) * thisbulb->desiredcolour + 220);
                    if(coap_execute(COAP_REQUEST_PUT, path, payload, &output) == 0){
                        thisbulb->desiredbrightness = NOCHANGE;
                        thisbulb->desiredcolour = NOCHANGE;
                    }else{
                        rc = 1;
                        numfails++;
                    }
                    if(output != NULL)
                        free(output);
                    
                }
            }
            break;
            case DEVICE_TYPE_OUTLET:
            {
                struct outlet *thisoutlet = (struct outlet *)(thisdev->typedata);
                if(thisoutlet->desiredpower != NOCHANGE){
                    sprintf(path, "15001/%d", thisdev->id);
                    sprintf(payload, SET_OUTLET_POWER, thisoutlet->desiredpower);
                    if(coap_execute(COAP_REQUEST_PUT, path, payload, &output) == 0){
                        thisoutlet->desiredpower = NOCHANGE;
                    }else{
                        rc = 1;
                        numfails++;
                    }
                    if(output != NULL)
                        free(output);
                }
            }
            break;
            case DEVICE_TYPE_BLIND:
            {
                struct blind *thisblind = (struct blind *)(thisdev->typedata);
                if(thisblind->desiredheight != NOCHANGE_FLOAT){
                    sprintf(path, "15001/%d", thisdev->id);
                    sprintf(payload, SET_BLIND_HEIGHT, thisblind->desiredheight);
                    if(coap_execute(COAP_REQUEST_PUT, path, payload, &output) == 0){
                        thisblind->desiredheight = NOCHANGE_FLOAT;
                    }else{
                        rc = 1;
                        numfails++;
                    }
                    if(output != NULL)
                        free(output);
                }
            }
            break;
            default:
                break;
        }
        thisdev = thisdev->next;
    }
    return rc;
}

/* Request a publish of all device states */
static int reportdevice(struct device *thisdev){
    bool justone = false;
    if(thisdev == NULL)
        thisdev = devicehead;
    else
        justone = true;
    while(thisdev != NULL){
        switch(thisdev->type){
            case DEVICE_TYPE_BULB:
            {
                struct bulb *thisbulb = (struct bulb *)(thisdev->typedata);
                thisbulb->reportpower = true;
                thisbulb->reportbrightness = true;
                thisbulb->reportmainson = true;
                if(thisbulb->iscolourtemp == true)
                    thisbulb->reportcolour = true;
            }
            break;
            case DEVICE_TYPE_OUTLET:
            {
                struct outlet *thisoutlet = (struct outlet *)(thisdev->typedata);
                thisoutlet->reportpower = true;
            }
            break;
            case DEVICE_TYPE_BLIND:
            {
                struct blind *thisblind = (struct blind *)(thisdev->typedata);
                thisblind->reportheight = true;
            }
            break;
            default:
                break;
        }
        if(thisdev->batterypowered == true)
            thisdev->reportbatterylevel = true;
        if(justone == true)
            break;
        thisdev = thisdev->next;
    }
    return 0;
}

/* Report each device parameter to a topic including the id and also to a topic including the name.
 * This means MQTT topics can be derived from the device names in the ikea app and the user
 * doesn't need to discover the device IDs */
static int rundevicereport(void){
    struct device *thisdev = devicehead;
    char topic[sizeof(statussubtopic) + strlen(basetopic) + strlen(thisdev->name) + 20];
    char message[20];
    while(thisdev != NULL){
        switch(thisdev->type){
            case DEVICE_TYPE_BULB:
            {
                struct bulb *thisbulb = (struct bulb *)(thisdev->typedata);
                if(thisbulb->reportpower == true){
                    sprintf(message, "%s", thisbulb->poweron == 1 ? "ON" : "OFF");
                    sprintf(topic, "%s%s%s/lamp", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/lamp", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s light %s\n", thisdev->name, message);
                    thisbulb->reportpower = false;
                }
                if(thisbulb->reportbrightness == true){
                    float realbright = (float)thisbulb->brightness / 2.54;
                    sprintf(message, "%d", (int)realbright);
                    sprintf(topic, "%s%s%s/brightness", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/brightness", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s brightness %s\n", thisdev->name, message);
                    thisbulb->reportbrightness = false;
                }
                if(thisbulb->reportcolour == true){
                    sprintf(message, "%d", thisbulb->colour);
                    sprintf(topic, "%s%s%s/temp", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/temp", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s colour temp %s\n", thisdev->name, message);
                    thisbulb->reportcolour = false;
                }
                if(thisbulb->reportmainson == true){
                    sprintf(message, "%s", thisbulb->mainson == 1 ? "ON" : "OFF");
                    sprintf(topic, "%s%s%s/power", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/power", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s mains %s\n", thisdev->name, message);
                    thisbulb->reportmainson = false;
                }
            }
            break;
            case DEVICE_TYPE_OUTLET:
            {
                struct outlet *thisoutlet = (struct outlet *)(thisdev->typedata);
                if(thisoutlet->reportpower == true){
                    sprintf(message, "%s", thisoutlet->poweron == 1 ? "ON" : "OFF");
                    sprintf(topic, "%s%s%s/power", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/power", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s outlet %s\n", thisdev->name, message);
                    thisoutlet->reportpower = false;
                }
                if(thisoutlet->reportmainson == true){
                    sprintf(message, "%s", thisoutlet->mainson == 1 ? "ON" : "OFF");
                    sprintf(topic, "%s%s%s/mains", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/mains", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s mains %s\n", thisdev->name, message);
                    thisoutlet->reportmainson = false;
                }
            }
            break;
            case DEVICE_TYPE_BLIND:
            {
                struct blind *thisblind = (struct blind *)(thisdev->typedata);
                /* Report the blind height */
                if(thisblind->reportheight == true){
                    sprintf(message, "%.1f", thisblind->height);
                    sprintf(topic, "%s%s%s/level", basetopic, statussubtopic, thisdev->name);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    sprintf(topic, "%s%s%d/level", basetopic, statussubtopic, thisdev->id);
                    mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
                    TRADFRILOG(LOG_CHANGE, "Send %s level %s\n", thisdev->name, message);
                    thisblind->reportheight = false;
                }
            }
            break;
            default:
                break;
        }
        /* Report the battery level and low boolean of any battery powered devices */
        if(thisdev->reportbatterylevel == true){
            sprintf(message, "%d", thisdev->batterylevel);
            sprintf(topic, "%s%s%s/battery", basetopic, statussubtopic, thisdev->name);
            mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
            sprintf(topic, "%s%s%d/battery", basetopic, statussubtopic, thisdev->id);
            mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
            TRADFRILOG(LOG_CHANGE, "Send %s battery %s\n", thisdev->name, message);
            sprintf(message, "%s", thisdev->batterylow == true ? "true" : "false");
            sprintf(topic, "%s%s%s/batterylow", basetopic, statussubtopic, thisdev->name);
            mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
            sprintf(topic, "%s%s%d/batterylow", basetopic, statussubtopic, thisdev->id);
            mosquitto_publish(m, NULL, topic, strlen(message), message, 0, true);
            TRADFRILOG(LOG_CHANGE, "Send %s batterylow %s\n", thisdev->name, message);
            thisdev->reportbatterylevel = false;
        }
        thisdev = thisdev->next;
    }
    return 0;
}

/* Read devices and groups */
static int check_gateway(void){
    char *output = NULL;
    int res;
    char path[200];
    struct device *dev = NULL;
    struct group *grp = NULL;
    
    /* Flag all devices as removed until we confirm they still exist */
    flagalldevicesremoved();
    
    /* Read the device list */
    sprintf(path, "15001");
    res = coap_execute(COAP_REQUEST_GET, path, NULL, &output);
    
    if(res == 0 && output != NULL){
        TRADFRILOG(LOG_DBG, "Get devices response: %s\n", output);
        char *finddevice = output;
        if(*finddevice == '['){
            int deviceid;
            finddevice++;
            while(finddevice != NULL && *finddevice != 0){
                deviceid = strtol(finddevice, &finddevice, 10);
                if(deviceid != 0){
                    dev = finddevid(deviceid);
                    if(dev == NULL){
                        TRADFRILOG(LOG_CHANGE, "Found new device id %d\n", deviceid);
                        newdevice(deviceid);
                    }else{
                        /* Confirm device still exists */
                        dev->removed = false;
                    }
                }
                if(finddevice != NULL){
                    if(*finddevice == ']')
                        break;
                    if(*finddevice == ',')
                        finddevice++;
                }
            }
        }
    }else{
        TRADFRILOG(LOG_ERR, "Error - Unable to poll devices\n");
        return res;
    }
    if(output != NULL){
        free(output);
        output = NULL;
    }
    
    /* Remove devices that are no longer reported by the gateway */
    dev = devicehead;
    while(dev != NULL){
        if(dev->removed == true){
            TRADFRILOG(LOG_CHANGE, "Device %d (%s) removed from gateway\n", dev->id, dev->name);
            deletedevice(dev);
        }
        dev = dev->next;
    }
    
    /* Flag all groups as removed until we confirm they still exist */
    flagallgroupsremoved();
      
    /* Read the group list */
    sprintf(path, "15004");
    res = coap_execute(COAP_REQUEST_GET, path, NULL, &output);
    
    if(res == 0 && output != NULL){
        TRADFRILOG(LOG_DBG, "Get groups response: %s\n", output);
        char *walkgroups = output;
        if(*walkgroups == '['){
            int groupid;
            walkgroups++;
            while(walkgroups != NULL && *walkgroups != 0){
                groupid = strtol(walkgroups, &walkgroups, 10);
                if(groupid != 0){
                    if((grp = findgroupid(groupid)) == NULL){
                        TRADFRILOG(LOG_CHANGE, "Found new group id %d\n", groupid);
                        newgroup(groupid);
                    }else{
                        /* Confirm group still exists */
                        grp->removed = false;
                    }
                }
                if(walkgroups != NULL){
                    if(*walkgroups == ']')
                        break;
                    if(*walkgroups == ',')
                        walkgroups++;
                }
            }
        }
    }else{
        TRADFRILOG(LOG_ERR, "Error - Unable to poll groups\n");
        return res;
    }
    if(output != NULL){
        free(output);
        output = NULL;
    }
    
    /* Remove groups that are no longer reported by the gateway */
    grp = grouphead;
    while(grp != NULL){
        if(grp->removed == true){
            TRADFRILOG(LOG_CHANGE, "Group %d (%s) removed from gateway\n", grp->id, grp->name);
            deletegroup(grp);
        }
        grp = grp->next;
    }
    
    return res;
}


int main(int argc, char **argv){
    int opt, res;  
    char *clientid = NULL;
    char *brokerurl = NULL;
    int mqttport = 1883;
    
    while ((opt = getopt(argc, argv, "ou:k:g:p:c:b:s:l:f:d:t:w:")) != -1) {
        switch (opt) {
            case 'u':
                user = strdup(optarg);
                userlen = strlen(user);
                break;
            case 'k':
                key = strdup(optarg);
                keylen = strlen(key);
                break;
            case 'g':
                gateway_url = strdup(optarg);
                break;
            case 'p':
                gateway_port = strtol(optarg, NULL, 10);
                break;
            case 'c':
                clientid = strdup(optarg);
                break;
            case 'b':
                brokerurl = strdup(optarg);
                break;
            case 's':
                mqttport = strtol(optarg, NULL, 10);
                break;
            case 'l':
                loglevel = strtol(optarg, NULL, 10);
                break;
            case 'f':
                logfile = strdup(optarg);
                break;
            case 'o':
                logfifo = true;
                break;
            case 'd':
                delaystart = strtol(optarg, NULL, 10);
                break;
            case 't':
                {
                    /* basetopic must have a '/' suffix - if none is supplied add one */
                    int len = strlen(optarg);
                    if(optarg[len - 1] == '/'){
                        basetopic = strdup(optarg);
                    }else{
                        char *newbase = malloc(len + 2);
                        if(newbase != NULL){
                            basetopic = malloc(len + 2);
                            sprintf(basetopic, "%s/", optarg);
                        }else{
                            printf("Failed to allocate base topic - using default %s\n", basetopic);
                        }
                    }
                }
                break;
            case 'w':
                wait_seconds = strtol(optarg, NULL, 10);
                break;
        }
    }
    
    setup_log();
    
    TRADFRILOG(LOG_ALWAYS, "Starting tradfri_mqttc\n", APP_VERSION);
    TRADFRILOG(LOG_ALWAYS, "tradfri_mqttc v%s\n", APP_VERSION);
    
    coap_dtls_set_log_level(log_level);
    coap_set_log_level(log_level);

    /* Intialise the polling semaphore */
    sem_init(&poll_sem, 0, 0);
    
    /* resolve destination address where server should be sent */
    res = resolve_host_address(gateway_url, &dst.addr.sa);
    if (res < 0) {
        TRADFRILOG(LOG_ERR, "failed to resolve tradfri gateway address\n");
        exit(-1);
    }
    
    dst.size = res;
    dst.addr.sin.sin_port = htons(gateway_port);

    /* Start MQTT connection */
    if(clientid != NULL && brokerurl != NULL){
        m = mqtt_init(&info, clientid, brokerurl, mqttport);
    }
    
    /* CoAP loop handling context reset.
     * IKEA periodically update the gateway software and a reset of the CoAP context is required when this happens */   
    while(1){
        TRADFRILOG(LOG_TRC, "Initialise CoAP context\n");
        numfails = 0;
        
        /* Create CoAP context */
        switch (dst.addr.sa.sa_family) {
        case AF_INET:
//        addrptr = &dst.addr.sin.sin_addr;

            /* create context for IPv4 */
            ctx = get_context("0.0.0.0", "0", 1);
            break;
        case AF_INET6:
//        addrptr = &dst.addr.sin6.sin6_addr;

            /* create context for IPv6 */
            ctx = get_context("::", "0", 1);
            break;
        default:
            ;
        }

        if (!ctx) {
            TRADFRILOG(LOG_ERR, "cannot create tradfri COAP context\n");
            exit(-1);
        }
    
        coap_keystore_item_t *psk;
        psk = coap_keystore_new_psk(NULL, 0, user, (size_t)userlen, key, (size_t)keylen, 0);
        if (!psk || !coap_keystore_store_item(ctx->keystore, psk, NULL)) {
            TRADFRILOG(LOG_ERR, "tradfrimqtt cannot store coaps key\n");
        }
    
        coap_register_option(ctx, COAP_OPTION_BLOCK2);
        coap_register_response_handler(ctx, message_handler);
    
        /* Discover devices and groups from gateway */
        check_gateway();  
    
        /* Read each device to populate read-once details and current values */
        struct device *devicewalk = devicehead;
        while(devicewalk != NULL){
            update_device(devicewalk);
            devicewalk = devicewalk->next;
        }
    
        /* Read each group to populate read-once details and current values */
        struct group *groupwalk = grouphead;
        while(groupwalk != NULL){
            update_group(groupwalk);
            groupwalk = groupwalk->next;
        }
    
        /* Holder for current time */
        struct timespec thistime;
        int pollcount = 0;
    
        clock_gettime(CLOCK_REALTIME, &thistime);
    
        struct timespec nextpolltime;
        /* Timeout @ GATEWAY_POLLTIME_S seconds from last poll */
        nextpolltime.tv_sec = thistime.tv_sec + GATEWAY_POLLTIME_S;
        nextpolltime.tv_nsec = thistime.tv_nsec;
        
        /* Main polling loop controlled by timer and semaphore */
        while(1){
        
            /* Wait on semaphore or timeout */
            sem_timedwait(&poll_sem, &nextpolltime);
        
            /* Obtain the current time */
            clock_gettime(CLOCK_REALTIME, &thistime);
        
            /* Push updates from mqtt to coap */
            rundeviceupdate();
        
            /* Only poll the tradfri gateway every GATEWAY_POLLTIME_S seconds */
            if(thistime.tv_sec > nextpolltime.tv_sec || (thistime.tv_sec == nextpolltime.tv_sec && thistime.tv_nsec >= nextpolltime.tv_nsec)){
            
                TRADFRILOG(LOG_TRC, "Runpoll\n");
            
                if(++pollcount >= GATEWAY_SCAN_POLLS){
                    pollcount = 0;
                    TRADFRILOG(LOG_TRC, "Inspect gateway devices\n");
                    /* Check all devices on the gateway. If an error occurs reset the CoAP context and restart polling */
                    if(0 != check_gateway())
                        break;
                }
            
                /* Read each device and check for changes */
                devicewalk = devicehead;
                while(devicewalk != NULL && numfails < 5){
                    update_device(devicewalk);
                    devicewalk = devicewalk->next;
                }
                /* Read each group and check for changes */
                groupwalk = grouphead;
                while(groupwalk != NULL && numfails < 5){
                    update_group(groupwalk);
                    groupwalk = groupwalk->next;
                }
                if(numfails < 5)
                    rundevicereport();
            
                /* Timeout @ GATEWAY_POLLTIME_S seconds from last poll */
                nextpolltime.tv_sec = thistime.tv_sec + GATEWAY_POLLTIME_S;
                nextpolltime.tv_nsec = thistime.tv_nsec;
            }
            if(numfails >= 5)
                break;
        }
        TRADFRILOG(LOG_ERR, "Polling errors occurred - reconnecting with gateway\n");
        coap_free_context(ctx);
        ctx = NULL;
    }
    return 0;
}

/* ================================================================== */
/* MQTT STUFF */

/* We *could* use different control topics for bulbs/outlets/blinds etc in future */
//#define LIGHTCONTROL 0
//#define OTHERCONTROL 1

#define CONTROL      0
#define CONTROL_GET  1

#define NUMCONTROLS  2

char *subtopic[NUMCONTROLS];
int submid[NUMCONTROLS];

static void create_subtopics(void){
    subtopic[CONTROL] = malloc(strlen(basetopic) + sizeof(setsubtopic) + 2);
    /* TODO check for NULL return */
    sprintf(subtopic[CONTROL], "%s%s#", basetopic, setsubtopic);
    subtopic[CONTROL_GET] = malloc(strlen(basetopic) + sizeof(getsubtopic) + 2);
    sprintf(subtopic[CONTROL_GET], "%s%s#", basetopic, getsubtopic);
}
    
/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
    if (res == 0) { /* success */
        //struct client_info *info = (struct client_info *)udata;
	    TRADFRILOG(LOG_CHANGE, "MQTT Connected\n");
        
        TRADFRILOG(LOG_TRC, "Subscribing to %s\n", subtopic[CONTROL]);
        mosquitto_subscribe(m, &submid[CONTROL], subtopic[CONTROL], 0);
        TRADFRILOG(LOG_TRC, "Subscribing to %s\n", subtopic[CONTROL_GET]);
        mosquitto_subscribe(m, &submid[CONTROL_GET], subtopic[CONTROL_GET], 0);
        
    } else {
        TRADFRILOG(LOG_ERR, "MQTT connection refused\n");
    }
}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
    
}

/* Handle a message that just arrived via one of the MQTT subscriptions. */
static void on_message(struct mosquitto *m, void *udata, const struct mosquitto_message *msg) {
    if (msg == NULL) 
        return; 
    TRADFRILOG(LOG_TRC, "-- got message @ %s: (%d, QoS %d, %s) '%s'\n", msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r", (char *)msg->payload);

    //topic:
    //<basetopic><setsubtopic><devaddr>/command
    //payload:
    //value
    
    int topiclen; 
    
    char *payload = malloc(msg->payloadlen + 1);
    if(payload != NULL)
        snprintf(payload, msg->payloadlen + 1, "%s", (char *)msg->payload);
    
    topiclen = strlen(subtopic[CONTROL]) - 1; /* subtopics always have a wildcard '#' at the end so remove that */
    if(strncmp(msg->topic, subtopic[CONTROL], topiclen) == 0){
        char *devaddr = &msg->topic[topiclen];
        if(devaddr != NULL){
            char *cmd = strchr(devaddr, '/');
            int devidlen = cmd - devaddr;
            char *devid = malloc(devidlen + 1);
            if(devid != NULL){
                enum _tradfri_cmd command = COMMAND_NONE;
                strncpy(devid, devaddr, devidlen);
                devid[devidlen] = 0;
                cmd++;
                /* devid is the device name/id and cmd is the command */
                TRADFRILOG(LOG_TRC, "%s request for device %s (value %s)\n", cmd, devid, payload);
                
                if(strncmp(cmd, "brightness", 10) == 0){
                    command = COMMAND_BRIGHTNESS;
                }else if(strncmp(cmd, "power", 5) == 0){
                    command = COMMAND_POWER;
                }else if(strncmp(cmd, "temp", 4) == 0){
                    command = COMMAND_COLOURTEMP;
                }else if(strncmp(cmd, "level", 5) == 0){
                    command = COMMAND_HEIGHT;
                }
                if(command == COMMAND_NONE)
                    return;
                
                struct group *groupset = findgroup(devid);
                if(groupset != NULL){
                    /* Pass the command on to the group set */
                    command_group(groupset, command, payload);
                }else{
                    struct device *devset = finddevice(devid);
                    if(devset != NULL){
                        /* Pass the command on to the device set */
                        command_device(devset, command, payload);
                    }
                }
                free(devid);
            }
        }
    }
    
    topiclen = strlen(subtopic[CONTROL_GET]) - 1; /* subtopics always have a wildcard '#' at the end so remove that */
    if(strncmp(msg->topic, subtopic[CONTROL_GET], topiclen) == 0){
        char *devaddr = &msg->topic[topiclen];
        if(*devaddr != 0){
            if(strcmp(devaddr, "all") != 0){
                struct device *devget = finddevice(devaddr);
                if(devget != NULL){
                    TRADFRILOG(LOG_TRC, "Report requested for %s\n", devget->name);
                    reportdevice(devget);
                }
            }else{
                /* If we have been requested to resend all details */
                TRADFRILOG(LOG_TRC, "Full report requested\n");
                reportdevice(NULL);
            }
        }
    }
    
    if(payload != NULL)
        free(payload);
    
    return;
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid, int qos_count, const int *granted_qos) {
    /* TODO Check submid[] array to see which subscription this refers to */
    /* Match mid to subrq */
    
    if(submid[CONTROL] == mid)
        TRADFRILOG(LOG_TRC, "Subscribed to set topic\n");
    else if(submid[CONTROL_GET] == mid)
        TRADFRILOG(LOG_TRC, "Subscribed to get topic\n");
    else
        TRADFRILOG(LOG_ERR, "Unknown subscription\n");
    
}

/* Register the callbacks that the mosquitto connection will use. */
static int set_callbacks(struct mosquitto *m) {
    mosquitto_connect_callback_set(m, on_connect);
    mosquitto_publish_callback_set(m, on_publish);
    mosquitto_subscribe_callback_set(m, on_subscribe);
    mosquitto_message_callback_set(m, on_message);
    return 0;
}

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct client_info *info) {
    int res = mosquitto_loop_forever(info->m, 1000, 1000 /* unused */);
    
    mosquitto_destroy(info->m);
    (void)mosquitto_lib_cleanup();
    
    if (res == MOSQ_ERR_SUCCESS) {
        return 0;
    } else {
        return 1;
    }
}

static void *mosqrun(void *data){
    struct client_info *cinfo = (struct client_info *)data;
    run_loop(cinfo);
    TRADFRILOG(LOG_ERR, "EEK mosquitto thread exiting!");
    return NULL;
}

/* Initialize a mosquitto client. */
static struct mosquitto *mqtt_init(struct client_info *info, const char *clientid, const char *mqtturl, int port) {
    void *udata = (void *)info;

    memset(info, 0, sizeof(struct client_info));
    
    struct mosquitto *mc = mosquitto_new(clientid, true, udata);
    set_callbacks(mc);
    
    info->m = mc;
    
    TRADFRILOG(LOG_TRC, "wait %d seconds to allow broker to start\n", delaystart);
    /* In case we get started before mosquitto wait a bit here */
    sleep(delaystart);
    TRADFRILOG(LOG_TRC, "Connect to mqtt broker...\n");
    while (mosquitto_connect(mc, mqtturl, port, 60) != MOSQ_ERR_SUCCESS) { 
        TRADFRILOG(LOG_ERR, "connect() failure - retry in 2 seconds ...\n"); 
        sleep(2);
    }
    mosquitto_threaded_set(mc, true);
    
    /* Create dynamic topics */
    create_subtopics();
    
    pthread_create(&mosq_thread, NULL, &mosqrun, (void *)info);
    
    TRADFRILOG(LOG_TRC, "MQTT client running\n");
    
    return mc;
}

