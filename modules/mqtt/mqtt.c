/**
 * @file mqtt.c  MQTT Remote Control
 *
 * Copyright (C) 2017 Erdem MEYDANLI
 */

#include <stdlib.h>
#include <time.h>
#include <re.h>
#include <baresip.h>
#include <MQTTClient.h>
#include <cjson/cJSON.h>
#include <cjson/cJSON_Utils.h>

/**
 * @defgroup mqtt mqtt
 *
 * MQTT Remote Control
 *
// * This module makes the Baresip application controllable via
 * MQTT protocol.
 */


/** Defines the status modes */
enum statmode {
    STATMODE_CALL = 0,
    STATMODE_OFF,
};

enum module_events {
    MQ_CONNECT,
    MQ_ANSWER,
    MQ_HANGUP,
    MQ_MUTE,
    MQ_UNMUTE,
    MQ_HOLD,
    MQ_RESUME, 
    MQ_CALL_STATUS,
    MQ_REGISTRATION_STATUS
};

static uint64_t start_ticks;          /**< Ticks when app started         */
static struct tmr tmr_alert;          /**< Incoming call alert timer      */
static struct tmr tmr_stat;           /**< Call status timer              */
static enum statmode statmode;        /**< Status mode                    */
static struct mbuf *dialbuf;          /**< Buffer for dialled number      */
static struct le *le_cur;             /**< Current User-Agent (struct ua) */

static struct {
    struct play *play;
    bool bell;

    struct tmr tmr_redial;        /**< Timer for auto-reconnect       */
    uint32_t redial_delay;        /**< Redial delay in [seconds]      */
    uint32_t redial_attempts;     /**< Number of re-dial attempts     */
    uint32_t current_attempts;    /**< Current number of re-dials     */

    struct mqueue *mq;
    MQTTClient client;
    MQTTClient_connectOptions connection_options;
} mqtt;

static const char *translate_errorcode(uint16_t scode)
{
    switch (scode) {

    case 404: return "notfound.wav";
    case 486: return "busy.wav";
    case 487: return NULL; /* ignore */
    default:  return "error.wav";
    }
}

static void check_registrations(void)
{
    static bool ual_ready = false;
    struct le *le;
    uint32_t n;

    if (ual_ready)
        return;

    for (le = list_head(uag_list()); le; le = le->next) {
        struct ua *ua = le->data;

        if (!ua_isregistered(ua))
            return;
    }

    n = list_count(uag_list());

    /* We are ready */
    ui_output("\x1b[32mAll %u useragent%s registered successfully!"
          " (%u ms)\x1b[;m\n",
          n, n==1 ? "" : "s",
          (uint32_t)(tmr_jiffies() - start_ticks));

    ual_ready = true;
}


/**
 * Return the current User-Agent in focus
 *
 * @return Current User-Agent
 */
static struct ua *uag_cur(void)
{
    return uag_current();
}

static int call_xfer(struct re_printf *pf, void *arg)
{
    const struct cmd_arg *carg = arg;
    static bool xfer_inprogress;

    if (!xfer_inprogress && !carg->complete) {
        statmode = STATMODE_OFF;
        re_hprintf(pf, "\rPlease enter transfer target SIP uri:\n");
    }

    xfer_inprogress = true;

    if (carg->complete) {
        statmode = STATMODE_CALL;
        xfer_inprogress = false;
        return call_transfer(ua_call(uag_cur()), carg->prm);
    }

    return 0;
}

static void mqtt_send_message(const char *msg)
{
    int ret = MQTTClient_isConnected(mqtt.client);

    if (1 == ret) {
        info("*** mqtt: %s\n", msg);
    } else {
        info("*** mqtt: connection failed!\n");
    }
}

static void ua_event_handler(struct ua *ua, enum ua_event ev,
                 struct call *call, const char *prm, void *arg)
{
    struct player *player = baresip_player();
    struct cmd_ctx *ctx = 0;
    struct commands *commands = baresip_commands();

    (void)call;
    (void)prm;
    (void)arg;

    switch (ev) {

    case UA_EVENT_CALL_INCOMING:
        mqtt_send_message("incoming call");

        /* set the current User-Agent to the one with the call */
        uag_current_set(ua);

        info("*** %s: Incoming call from: %s %s -"
             " (press 'a' to accept)\n",
             ua_aor(ua), call_peername(call), call_peeruri(call));

        /* stop any ringtones */
        mqtt.play = mem_deref(mqtt.play);

        /* Only play the ringtones if answermode is "Manual".
         * If the answermode is "auto" then be silent.
         */
        if (ANSWERMODE_MANUAL == account_answermode(ua_account(ua))) {

            if (list_count(ua_calls(ua)) > 1) {
                (void)play_file(&mqtt.play, player,
                        "callwaiting.wav", 3);
            }
            else {
                /* Alert user */
                (void)play_file(&mqtt.play, player,
                        "ring.wav", -1);
            }
        }

        break;

    case UA_EVENT_CALL_RINGING:
        mqtt_send_message("bell is ringing");
        /* stop any ringtones */
        mqtt.play = mem_deref(mqtt.play);

        (void)play_file(&mqtt.play, player, "ringback.wav", -1);
        break;

    case UA_EVENT_CALL_ESTABLISHED:
        mqtt_send_message("connection established");
        /* stop any ringtones */
        mqtt.play = mem_deref(mqtt.play);
        break;

    case UA_EVENT_CALL_CLOSED:
        /* stop any ringtones */
        mqtt_send_message("connection closed");

        mqtt.play = mem_deref(mqtt.play);

        if (call_scode(call)) {
            const char *tone;
            tone = translate_errorcode(call_scode(call));
            if (tone) {
                (void)play_file(&mqtt.play, player,
                        tone, 1);
            }
        }
        break;

    case UA_EVENT_REGISTER_OK:
        mqtt_send_message("registration: OK");
        check_registrations();
        break;

    case UA_EVENT_UNREGISTERING:
        mqtt_send_message("unregistration: OK");
        return;

    default:
        break;
    }
}

static void mqueue_handler(int id, void *data, void *arg)
{
    switch ((enum module_events)id) {
    case MQ_ANSWER: 
        {
            struct ua *ua = uag_cur();

            info ("Answering incoming call: %s\n", ua_aor(ua));

            /* Stop any ongoing ring-tones */
            mqtt.play = mem_deref(mqtt.play);

            ua_hold_answer(ua, NULL);
        }
        break;

    case MQ_HANGUP:
        {
            mqtt_send_message("{\"event\":\"hangup\"}");
            usleep(10);

            /* Stop any ongoing ring-tones */
            mqtt.play = mem_deref(mqtt.play);

            ua_hangup(uag_cur(), NULL, 0, NULL);
        }
        break;

    case MQ_CONNECT:
        {
            //mqtt_send_message("{\"event\":\"connected\"}");
            const char *uri = (const char *) data;
            printf("URL is: %s\n", uri);
            ua_connect(uag_cur(), NULL, NULL, uri, NULL, VIDMODE_OFF);
        }
        break;
    
    case MQ_MUTE:
        {
            struct audio *audio = call_audio(ua_call(uag_cur()));        
            audio_mute(audio, true);
            mqtt_send_message("{\"event\":\"muted\"}");
        }
        break;

    case MQ_UNMUTE:
        {
            struct audio *audio = call_audio(ua_call(uag_cur()));        
            audio_mute(audio, false);
            mqtt_send_message("{\"event\":\"unmuted\"}");
        }
        break;

    case MQ_HOLD:
        {
            int ret = call_hold(ua_call(uag_cur()), true);
            info("Hold result: %d\n", ret);
        }
        break;

    case MQ_RESUME:
        {
            int ret = call_hold(ua_call(uag_cur()), false);
            info("unhold result: %d\n", ret);
        }
        break;

    case MQ_CALL_STATUS:
        {
            int status = ua_call(uag_cur());
            if (status)
                mqtt_send_message("{\"active_call\":\"yes\"}");
            else
                mqtt_send_message("{\"active_call\":\"no\"}");
        }
        break;

    case MQ_REGISTRATION_STATUS:
        {
            int status = list_count(uag_list());
            if (status)
                mqtt_send_message("{\"registered\":\"yes\"}");
            else
                mqtt_send_message("{\"registered\":\"no\"}");
        }
        break;
#if 0
    case MQ_TRANSFER:
        call_transfer(win->call, data);
        break;
#endif
    }
}

static void message_handler(const struct pl *peer, const struct pl *ctype,
                struct mbuf *body, void *arg)
{
    (void)ctype;
    (void)arg;

    (void)re_fprintf(stderr, "\r%r: \"%b\"\n", peer,
             mbuf_buf(body), mbuf_get_left(body));

    (void)play_file(NULL, baresip_player(), "message.wav", 0);
}

static int mqtt_message_arrived(void *context, char *topicName, int topicLen, MQTTClient_message *message)
{
    char *str = 0;
    //printf("Topic Name: %s\n", topicName);

    if (!strncmp(topicName, "baresip/read", topicLen)) {
        str = (char *) calloc(1, message->payloadlen + 1);
        if (!str) return 0;

        memcpy(str, message->payload, message->payloadlen);
        info("mqtt: raw data: %s\n", str);
        cJSON *json = cJSON_Parse(str);

        if (json) {
            int cmd = cJSON_GetObjectItem(json,"command")->valueint;
            info("current command is: %d\n", cmd);

            switch (cmd) {
            case 'a':
                mqueue_push(mqtt.mq, MQ_ANSWER, NULL);
                break;
            case 'b':
                mqueue_push(mqtt.mq, MQ_HANGUP, NULL);
                break;
            case 'd':
                {
                    cJSON *item = cJSON_GetObjectItem(json, "account");
                    if (NULL != item) {
                        if (item->valuestring && item->valuestring[0] != '\0') {
                            char *arg = (char *) malloc(32);
                            strcpy(arg, item->valuestring);
                            info("mqtt: connecting to %s\n", arg);
                            mqueue_push(mqtt.mq, MQ_CONNECT, arg);
                        }
                    } else {
                        info("mqtt: no account is specified for the call.\n");
                    }
                }
                break;
            case 'u':
                mqueue_push(mqtt.mq, MQ_UNMUTE, NULL);
                mqtt_send_message("audio unmuted");
                break;
            case 'm': 
                mqueue_push(mqtt.mq, MQ_MUTE, NULL);;
                mqtt_send_message("audio muted");
                break;
            case 'h': 
                mqueue_push(mqtt.mq, MQ_HOLD, NULL);;
                break;
            case 'r': 
                mqueue_push(mqtt.mq, MQ_RESUME, NULL);;
                break;
            case 's':
                /* call status */
                mqueue_push(mqtt.mq, MQ_CALL_STATUS, NULL);
                break;
            case 'p':
                /* registration status */
                mqueue_push(mqtt.mq, MQ_REGISTRATION_STATUS, NULL);            
                break;
            default:
                info("mqtt: message not recognized!\n");
            }
        } else {
            info("mqtt: invalid command!\n");
        }
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    free(str);

    return 1;
}

static void mqtt_connection_lost(void *context, char *cause)
{
    printf("\nConnection lost\n");
    printf("     cause: %s\n", cause);
}

static int module_init(void)
{
    struct pl val;
    int err;
    MQTTClient_connectOptions connection_options = MQTTClient_connectOptions_initializer;

    (void) call_xfer;

    printf("Initializing module mqtt!\n");

    err = mqueue_alloc(&mqtt.mq, mqueue_handler, &mqtt);
    if (err)
        return err;

    statmode = STATMODE_CALL;

    err |= uag_event_register(ua_event_handler, NULL);
    err |= message_init(message_handler, NULL);

    /* TODO: load hardcoded values from the configuration file */
    MQTTClient_create(&mqtt.client, "localhost", "meerd", MQTTCLIENT_PERSISTENCE_NONE, NULL);
    memcpy(&mqtt.connection_options, &connection_options, sizeof(connection_options));

    mqtt.connection_options.keepAliveInterval = 20;
    mqtt.connection_options.cleansession = 1;

    MQTTClient_setCallbacks(mqtt.client, NULL, mqtt_connection_lost, mqtt_message_arrived, NULL);

    err = MQTTClient_connect(mqtt.client, &mqtt.connection_options);

    if (err != MQTTCLIENT_SUCCESS) {
        info("Error while connecting to mqtt broker...\n");
    }

    MQTTClient_subscribe(mqtt.client, "baresip/read", 0);
    return err;
}


static int module_close(void)
{
    debug("info: close (redial current_attempts=%d)\n",
          mqtt.current_attempts);

    message_close();
    uag_event_unregister(ua_event_handler);

    mqtt.play = mem_deref(mqtt.play);

    return 0;
}


const struct mod_export DECL_EXPORTS(menu) = {
    "mqtt",
    "application",
    module_init,
    module_close
};
