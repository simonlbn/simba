/**
 * @section License
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014-2017, Erik Moqvist
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * This file is part of the Simba project.
 */

#include "simba.h"

/**
 * Implements MQTT version 3.11.
 */

/** Control packet types. */
#define MQTT_CONNECT      1
#define MQTT_CONNACK      2
#define MQTT_PUBLISH      3
#define MQTT_PUBACK       4
#define MQTT_PUBREC       5
#define MQTT_PUBREL       6
#define MQTT_PUBCOMP      7
#define MQTT_SUBSCRIBE    8
#define MQTT_SUBACK       9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK    11
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

/** Connection flags. */
#define CLEAN_SESSION   0x2
#define WILL_FLAG       0x4
#define WILL_QOS_1      0x8
#define WILL_QOS_2      0x10
#define WILL_RETAIN     0x20
#define PASSWORD_FLAG   0x40
#define USER_NAME_FLAG  0x80

#define CONNECTION_ACCEPTED 0

#define CONTROL_CONNECT        0
#define CONTROL_DISCONNECT     1
#define CONTROL_PING           2
#define CONTROL_PUBLISH        3
#define CONTROL_SUBSCRIBE      4
#define CONTROL_UNSUBSCRIBE    5
#define CONTROL_NONE           6

static const char *message_fmt[] = {
    "forbidden",
    "connect",
    "connack",
    "publish",
    "puback",
    "pubrec",
    "pubrel",
    "pubcomp",
    "subscribe",
    "suback",
    "unsubscribe",
    "unsuback",
    "pingreq",
    "pingresp",
    "disconnect",
    "forbidden"
};

//! Interval required between MQTT packets.
#define KEEP_ALIVE 300

/*! Extract Most Significant Byte from a 16bit value. */
#define MSB(b) ((b >> 8) & 0xff)

/*! Extract Least Significant Byte from a 16bit value. */
#define LSB(b) (b & 0xff)

/**
 * Write a single variable length string with header to the server.
 */
static int write_mqtt_string(struct mqtt_client_t *self_p,
                             struct mqtt_string_t *mqtt_string)
{
    int res;
    uint8_t buf[2];

    if (mqtt_string->size == 0 || mqtt_string->buf_p == NULL) {
        return (-EINVAL);
    }

    if (mqtt_string->size > 0xffff) {
        return (-EINVAL);
    }

    /* Write length header */
    buf[0] = MSB(mqtt_string->size);
    buf[1] = LSB(mqtt_string->size);

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    res = chan_write(self_p->transport.out_p,
                     mqtt_string->buf_p,
                     mqtt_string->size);

    if (res != mqtt_string->size) {
        return (-EIO);
    }

    return (0);
}

/**
 * Write the fixed header of the MQTT message to the server.
 */
static int write_fixed_header(struct mqtt_client_t *self_p,
                              int type,
                              int flags,
                              size_t size)
{
    uint8_t buf[5];
    int pos;
    uint8_t encoded_byte;

    log_object_print(self_p->log_object_p,
                     LOG_DEBUG,
                     OSTR("Writing MQTT message '%s' to the server.\r\n"),
                     message_fmt[type]);

    buf[0] = (type << 4) | flags;
    pos = 1;

    do {
        /* Encode the variable length size field. */
        encoded_byte = (size % 128);
        size /= 128;

        /* If there are more data to encode, set the top bit of this
           byte. */
        if (size > 0) {
            encoded_byte |= 0x80;
        }

        buf[pos] = encoded_byte;
        pos++;
    } while (size > 0);

    if (chan_write(self_p->transport.out_p, &buf[0], pos) != pos) {
        return (-EIO);
    }

    return (0);
}

/**
 * Read the fixed header of a MQTT message from the server.
 */
static int read_fixed_header(struct mqtt_client_t *self_p,
                             int *type_p,
                             int *flags_p,
                             size_t *size_p)
{
    uint8_t byte;
    long multiplier;

    if (chan_read(self_p->transport.in_p, &byte, 1) != 1) {
        return (-EIO);
    }

    *type_p = ((byte >> 4) & 0xf);
    *flags_p = (byte & 0xf);

    /* Read the variablie size field. */
    multiplier = 1;
    *size_p = 0;

    do {
        if (chan_read(self_p->transport.in_p, &byte, 1) != 1) {
            return (-EIO);
        }

        *size_p += (byte & 0x7f) * multiplier;
        multiplier *= 128;

        if (multiplier > 128L * 128L * 128L) {
            return (-1);
        }
    } while ((byte & 0x80) != 0);

    return (0);
}

/**
 * Send the connect message to the server.
 */
static int handle_control_connect(struct mqtt_client_t *self_p)
{
    struct mqtt_conn_options_t *options_p;
    struct mqtt_conn_options_t default_options;
    int res = 0, payload_length = 0;
    uint8_t buf[12], flags = 0;

    /*
     * Note: Each payload string requires a 2 byte length header, so
     * that must be accounted for in the payload length (hence + 2
     * number of places below).
     */

    if (queue_read(&self_p->control.in,
                   &options_p,
                   sizeof(options_p)) != sizeof(options_p)) {
        return (-1);
    }

    if (options_p == NULL) {
        options_p = &default_options;
        memset(options_p, 0, sizeof(*options_p));
    }

    /*
     * We currently do not support ressuming session, so force clean
     * session.
     */
    flags = CLEAN_SESSION;

    /* Be sure that 'will' topic and payload are both either set or unset */
    if ((options_p->will.topic.size == 0) !=
        (options_p->will.payload.size == 0)) {
        return (-EINVAL);
    }

    /*
     * As per MQTT-3.1.3-3 a Client ID is required, so if the user has
     * not specified one, we set one.
     */
    if (options_p->client_id.size == 0) {
        options_p->client_id.buf_p = FSTR("simba_mqtt");
        options_p->client_id.size = strlen(options_p->client_id.buf_p);
    }

    /* Calculate payload length, and set flags. */
    payload_length = options_p->client_id.size + 2;

    if (options_p->will.topic.size > 0) {
        flags |= WILL_FLAG;

        if (options_p->will.qos == mqtt_qos_1_t) {
            flags |= WILL_QOS_1;
        } else if (options_p->will.qos == mqtt_qos_2_t) {
            flags |= WILL_QOS_2;
        }

        payload_length += options_p->will.topic.size + 2;
        payload_length += options_p->will.payload.size + 2;
    }

    if (options_p->user_name.size > 0) {
        flags |= USER_NAME_FLAG;
        payload_length += options_p->user_name.size + 2;
    }

    if (options_p->password.size > 0) {
        flags |= PASSWORD_FLAG;
        payload_length += options_p->password.size + 2;
    }

    std_printf("payload = %d\r\n", payload_length);

    /* Write the fixed header. */
    res = write_fixed_header(self_p, MQTT_CONNECT, 0, 12 + payload_length);

    if (res != 0) {
        return (res);
    }

    /* Write the variable header. */
    buf[0] = 0;                          /* Protocol Name - Length MSB */
    buf[1] = 4;                          /* Protocol Name - Length LSB */
    buf[2] = 'M';                        /* Protocol Name */
    buf[3] = 'Q';                        /* Protocol Name */
    buf[4] = 'T';                        /* Protocol Name */
    buf[5] = 'T';                        /* Protocol Name */
    buf[6] = 4;                          /* Protocol Level */
    buf[7] = flags;                      /* Connect Flags */
    buf[8] = MSB(KEEP_ALIVE);            /* Keep Alive MSB */
    buf[9] = LSB(KEEP_ALIVE);            /* Keep Alive LSB */
    buf[10] = MSB(payload_length);       /* Payload - Length MSB */
    buf[11] = LSB(payload_length);       /* Payload - Length LSB */

    if (chan_write(self_p->transport.out_p, &buf[0], 12) != 12) {
        return (-EIO);
    }

    /* Write paylaod */
    res = write_mqtt_string(self_p, &options_p->client_id);

    if (res != 0) {
        return (res);
    }

    if (options_p->will.topic.size > 0) {
        res = write_mqtt_string(self_p, &options_p->will.topic);

        if (res != 0) {
            return (res);
        }

        res = write_mqtt_string(self_p, &options_p->will.payload);

        if (res != 0) {
            return (res);
        }
    }

    if (options_p->user_name.size > 0) {
        res = write_mqtt_string(self_p, &options_p->user_name);

        if (res != 0) {
            return (res);
        }
    }

    if (options_p->password.size > 0) {
        res = write_mqtt_string(self_p, &options_p->password);

        if (res != 0) {
            return (res);
        }
    }

    self_p->message.type = CONTROL_CONNECT;

    return (0);
}

/**
 * Handle the connack message from the server.
 */
static int handle_response_connack(struct mqtt_client_t *self_p,
                                   size_t size)
{
    char buf[2];

    if (self_p->message.type != CONTROL_CONNECT) {
        return (-1);
    }

    self_p->message.type = CONTROL_NONE;

    if (size != 2) {
        return (-EMSGSIZE);
    }

    if (chan_read(self_p->transport.in_p, &buf[0], size) != size) {
        return (-EIO);
    }

    if (buf[0] != 0) {
        return (-1);
    }

    if (buf[1] != CONNECTION_ACCEPTED) {
        return (-1);
    }

    self_p->state = mqtt_client_state_connected_t;

    return (0);
}

/**
 * Send the disconnect message to the server.
 */
static int handle_control_disconnect(struct mqtt_client_t *self_p)
{

    if (write_fixed_header(self_p, MQTT_DISCONNECT, 0, 0) != 0) {
        return (-1);
    }

    self_p->state = mqtt_client_state_disconnected_t;

    return (0);
}

/**
 * Send the ping message to the server.
 */
static int handle_control_ping(struct mqtt_client_t *self_p)
{
    int res;

    /* Write the ping request packet. */
    res = write_fixed_header(self_p, MQTT_PINGREQ, 0, 0);

    if (res != 0) {
        return (res);
    }

    self_p->message.type = CONTROL_PING;

    return (0);
}

/**
 * Handle the ping message from the server.
 */
static int handle_response_ping(struct mqtt_client_t *self_p,
                                size_t size)
{
    if (self_p->message.type != CONTROL_PING) {
        return (-1);
    }

    self_p->message.type = CONTROL_NONE;

    if (size != 0) {
        return (-EMSGSIZE);
    }

    return (0);
}

/**
 * Send the publish message to the server.
 */
static int handle_control_publish(struct mqtt_client_t *self_p)
{
    int res = 0;
    uint8_t buf[2];
    struct mqtt_application_message_t *message_p;
    size_t size;

    if (queue_read(&self_p->control.in,
                   &message_p,
                   sizeof(message_p)) != sizeof(message_p)) {
        return (-1);
    }

    /* Write the fixed header. */
    size = (message_p->topic.size + message_p->payload.size + 2);

    if (message_p->qos > 0) {
        size += 2;
    }

    res = write_fixed_header(self_p,
                             MQTT_PUBLISH,
                             (message_p->qos << 1),
                             size);

    if (res != 0) {
        return (res);
    }

    /* Write the variable header. */
    buf[0] = 0;
    buf[1] = message_p->topic.size;

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    if (chan_write(self_p->transport.out_p,
                   message_p->topic.buf_p,
                   message_p->topic.size) != message_p->topic.size) {
        return (-EIO);
    }

    if (message_p->qos > 0) {
        buf[0] = 0;
        buf[1] = 1;

        if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
            return (-EIO);
        }
    }

    /* Write the payload. */
    if (message_p->payload.size > 0) {
        if (chan_write(self_p->transport.out_p,
                       message_p->payload.buf_p,
                       message_p->payload.size) != message_p->payload.size) {
            return (-EIO);
        }
    }

    self_p->message.type = CONTROL_PUBLISH;

    return (0);
}

/**
 * Handle the puback message from the server.
 */
static int handle_response_puback(struct mqtt_client_t *self_p,
                                  size_t size)
{
    uint8_t buf[2];

    if (self_p->message.type != CONTROL_PUBLISH) {
        return (-1);
    }

    self_p->message.type = CONTROL_NONE;

    if (size != 2) {
        return (-EMSGSIZE);
    }

    if (chan_read(self_p->transport.in_p, &buf[0], size) != size) {
        return (-EIO);
    }

    if (buf[0] != 0) {
        return (-1);
    }

    if (buf[1] != 1) {
        return (-1);
    }

    return (0);
}

/**
 * Send the subscribe message to the server.
 */
static int handle_control_subscribe(struct mqtt_client_t *self_p)
{
    int res = 0;
    uint8_t buf[2];
    struct mqtt_application_message_t *message_p;

    if (queue_read(&self_p->control.in,
                   &message_p,
                   sizeof(message_p)) != sizeof(message_p)) {
        return (-1);
    }

    /* Write the fixed header to the server. */
    res = write_fixed_header(self_p,
                             MQTT_SUBSCRIBE,
                             2,
                             message_p->topic.size + 5);

    if (res != 0) {
        return (res);
    }

    /* Write the packet identifier. */
    buf[0] = 0;
    buf[1] = 1;

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    /* Write the topic filter length. */
    buf[0] = ((message_p->topic.size >> 8) & 0xff);
    buf[1] = (message_p->topic.size & 0xff);

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    /* Write the topic filter. */
    if (chan_write(self_p->transport.out_p,
                   message_p->topic.buf_p,
                   message_p->topic.size) != message_p->topic.size) {
        return (-EIO);
    }

    /* Write the topic filter QoS. */
    buf[0] = message_p->qos;

    if (chan_write(self_p->transport.out_p, &buf[0], 1) != 1) {
        return (-EIO);
    }

    self_p->message.type = CONTROL_SUBSCRIBE;

    return (0);
}

/**
 * Handle the suback message from the server.
 */
static int handle_response_suback(struct mqtt_client_t *self_p,
                                  size_t size)
{
    uint8_t buf[3];

    if (self_p->message.type != CONTROL_SUBSCRIBE) {
        return (-1);
    }

    self_p->message.type = CONTROL_NONE;

    if (size != 3) {
        return (-EMSGSIZE);
    }

    if (chan_read(self_p->transport.in_p, &buf[0], size) != size) {
        return (-EIO);
    }

    if (buf[0] != 0) {
        return (-1);
    }

    if (buf[1] != 1) {
        return (-1);
    }

    if (buf[2] > 2) {
        return (-1);
    }

    return (0);
}

/**
 * Send the unsubscribe message to the server.
 */
static int handle_control_unsubscribe(struct mqtt_client_t *self_p)
{
    int res = 0;
    uint8_t buf[2];
    struct mqtt_application_message_t *message_p;

    if (queue_read(&self_p->control.in,
                   &message_p,
                   sizeof(message_p)) != sizeof(message_p)) {
        return (-1);
    }

    /* Write the fixed header to the server. */
    res = write_fixed_header(self_p,
                             MQTT_UNSUBSCRIBE,
                             2,
                             message_p->topic.size + 4);

    if (res != 0) {
        return (res);
    }

    /* Write the packet identifier. */
    buf[0] = 0;
    buf[1] = 2;

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    /* Write the topic filter length. */
    buf[0] = ((message_p->topic.size >> 8) & 0xff);
    buf[1] = (message_p->topic.size & 0xff);

    if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
        return (-EIO);
    }

    /* Write the topic filter. */
    if (chan_write(self_p->transport.out_p,
                   message_p->topic.buf_p,
                   message_p->topic.size) != message_p->topic.size) {
        return (-EIO);
    }

    self_p->message.type = CONTROL_UNSUBSCRIBE;

    return (0);
}

/**
 * Handle the unsuback message from the server.
 */
static int handle_response_unsuback(struct mqtt_client_t *self_p,
                                    size_t size)
{
    uint8_t buf[3];

    if (self_p->message.type != CONTROL_UNSUBSCRIBE) {
        return (-1);
    }

    self_p->message.type = CONTROL_NONE;

    if (size != 2) {
        return (-EMSGSIZE);
    }

    if (chan_read(self_p->transport.in_p, &buf[0], size) != size) {
        return (-EIO);
    }

    if (buf[0] != 0) {
        return (-1);
    }

    if (buf[1] != 2) {
        return (-1);
    }

    return (0);
}

/**
 * Handle the publish message from the server.
 */
static int handle_publish(struct mqtt_client_t *self_p,
                          size_t size,
                          int flags)
{
    int res;
    size_t topic_size;
    size_t payload_size;
    uint8_t buf[2];
    uint8_t qos;
    char topic[128];

    /* Read the variable header. */
    if (chan_read(self_p->transport.in_p, buf, 2) != 2) {
        return (-EIO);
    }

    topic_size = (((size_t)buf[0] << 8) | buf[1]);

    if (topic_size > sizeof(topic) - 1) {
        return (-EMSGSIZE);
    }

    /* Read the topic. */
    if (chan_read(self_p->transport.in_p,
                  topic,
                  topic_size) != topic_size) {
        return (-EIO);
    }

    topic[topic_size] = '\0';
    qos = ((flags >> 1) & 0x3);

    log_object_print(self_p->log_object_p,
                     LOG_DEBUG,
                     OSTR("QoS: %d, Flags: 0x%02x.\r\n"),
                     qos,
                     flags);

    if (qos == 0) {
        payload_size = (size - topic_size - 2);
    } else {
        /* Read the packet identifier. */
        if (chan_read(self_p->transport.in_p, buf, 2) != 2) {
            return (-EIO);
        }

        if (qos == 1) {
            res = write_fixed_header(self_p, MQTT_PUBACK, 0, 2);
        } else if (qos == 2) {
            res = write_fixed_header(self_p, MQTT_PUBREC, 0, 2);
        } else {
            res = (-EPROTO);
        }

        if (res != 0) {
            return (res);
        }

        /* Write the variable header. */
        if (chan_write(self_p->transport.out_p, &buf[0], 2) != 2) {
            return (-EIO);
        }

        payload_size = (size - topic_size - 4);
    }

    if (self_p->on_publish(self_p,
                           topic,
                           self_p->transport.in_p,
                           payload_size) != 0) {
        return (-1);
    }

    return (0);
}

/**
 * Read a control message.
 */
static int read_control_message(struct mqtt_client_t *self_p)
{
    int res = -1;
    char type;

    if (queue_read(&self_p->control.in,
                   &type,
                   sizeof(type)) != sizeof(type)) {
        return (-1);
    }

    switch (self_p->state) {

    case mqtt_client_state_disconnected_t:
        {
            switch (type) {

            case CONTROL_CONNECT:
                res = handle_control_connect(self_p);
                break;

            default:
                break;
            }
        }
        break;

    case mqtt_client_state_connected_t:
        {
            switch (type) {

            case CONTROL_DISCONNECT:
                res = handle_control_disconnect(self_p);
                chan_write(&self_p->control.out, &res, sizeof(res));
                break;

            case CONTROL_PING:
                res = handle_control_ping(self_p);
                break;

            case CONTROL_PUBLISH:
                res = handle_control_publish(self_p);
                break;

            case CONTROL_SUBSCRIBE:
                res = handle_control_subscribe(self_p);
                break;

            case CONTROL_UNSUBSCRIBE:
                res = handle_control_unsubscribe(self_p);
                break;

            default:
                break;
            }
        }
        break;

    default:
        break;
    }

    return (0);
}

/**
 * Read a MQTT message from the server.
 */
static int read_server_message(struct mqtt_client_t *self_p)
{
    int res;
    int type;
    int flags;
    size_t size;

    res = 0;
    flags = 0;
    size = 0;

    if (read_fixed_header(self_p, &type, &flags, &size) != 0) {
        return (-EIO);
    }

    log_object_print(self_p->log_object_p,
                     LOG_DEBUG,
                     OSTR("Read MQTT message '%s' from the server.\r\n"),
                     message_fmt[type]);

    switch (type) {

    case MQTT_CONNACK:
        res = handle_response_connack(self_p,  size);
        chan_write(&self_p->control.out, &res, sizeof(res));
        break;

    case MQTT_PUBACK:
        res = handle_response_puback(self_p, size);
        chan_write(&self_p->control.out, &res, sizeof(res));
        break;

    case MQTT_PUBREC:
    case MQTT_PUBREL:
    case MQTT_PUBCOMP:
        break;

    case MQTT_SUBACK:
        res = handle_response_suback(self_p,  size);
        chan_write(&self_p->control.out, &res, sizeof(res));
        break;

    case MQTT_UNSUBACK:
        res = handle_response_unsuback(self_p,  size);
        chan_write(&self_p->control.out, &res, sizeof(res));
        break;

    case MQTT_PINGRESP:
        res = handle_response_ping(self_p, size);
        chan_write(&self_p->control.out, &res, sizeof(res));
        break;

    case MQTT_PUBLISH:
        res = handle_publish(self_p, size, flags);
        break;

    default:
        break;
    }

    return (res);
}

/**
 * Default on_error callback used if application has not provided one.
 */
static int default_on_error(struct mqtt_client_t *self_p,
                            int error)
{
    log_object_print(self_p->log_object_p,
                     LOG_ERROR,
                     OSTR("mqtt_client error: %d.\r\n"),
                     error);

    return (0);
}

int mqtt_client_init(struct mqtt_client_t *self_p,
                     const char *name_p,
                     struct log_object_t *log_object_p,
                     void *transport_out_p,
                     void *transport_in_p,
                     mqtt_on_publish_t on_publish,
                     mqtt_on_error_t on_error)
{
    if (on_error == NULL) {
        on_error = default_on_error;
    }

    self_p->name_p = name_p;
    self_p->log_object_p = log_object_p;
    self_p->state = mqtt_client_state_disconnected_t;
    self_p->message.type = CONTROL_NONE;
    self_p->transport.out_p = transport_out_p;
    self_p->transport.in_p = transport_in_p;
    queue_init(&self_p->control.out, NULL, 0);
    queue_init(&self_p->control.in, NULL, 0);
    self_p->on_publish = on_publish;
    self_p->on_error = on_error;

    return (0);
}

static int control_routine(struct mqtt_client_t *self_p,
                           char type,
                           void *buf_p,
                           size_t size)
{
    int res;

    queue_write(&self_p->control.in, &type, sizeof(type));

    if (size > 0) {
        queue_write(&self_p->control.in, buf_p, size);
    }

    queue_read(&self_p->control.out, &res, sizeof(res));

    return (res);
}

int mqtt_client_connect(struct mqtt_client_t *self_p,
                        struct mqtt_conn_options_t *options_p)
{
    ASSERTN(self_p != NULL, EINVAL)

    return (control_routine(self_p,
                            CONTROL_CONNECT,
                            &options_p,
                            sizeof(options_p)));
}

int mqtt_client_disconnect(struct mqtt_client_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL)

    return (control_routine(self_p, CONTROL_DISCONNECT, NULL, 0));
}

int mqtt_client_ping(struct mqtt_client_t *self_p)
{
    ASSERTN(self_p != NULL, EINVAL)

    return (control_routine(self_p, CONTROL_PING, NULL, 0));
}

int mqtt_client_publish(struct mqtt_client_t *self_p,
                        struct mqtt_application_message_t *message_p)
{
    ASSERTN(self_p != NULL, EINVAL)
    ASSERTN(message_p != NULL, EINVAL)

    return (control_routine(self_p,
                            CONTROL_PUBLISH,
                            &message_p,
                            sizeof(message_p)));
}

int mqtt_client_subscribe(struct mqtt_client_t *self_p,
                        struct mqtt_application_message_t *message_p)
{
    ASSERTN(self_p != NULL, EINVAL)
    ASSERTN(message_p != NULL, EINVAL)

    return (control_routine(self_p,
                            CONTROL_SUBSCRIBE,
                            &message_p,
                            sizeof(message_p)));
}

int mqtt_client_unsubscribe(struct mqtt_client_t *self_p,
                        struct mqtt_application_message_t *message_p)
{
    ASSERTN(self_p != NULL, EINVAL)
    ASSERTN(message_p != NULL, EINVAL)

    return (control_routine(self_p,
                            CONTROL_UNSUBSCRIBE,
                            &message_p,
                            sizeof(message_p)));
}

void *mqtt_client_main(void *arg_p)
{
    struct mqtt_client_t *self_p = arg_p;
    struct chan_list_t list;
    struct chan_list_elem_t elements[2];
    void *chan_p;
    int res;

    thrd_set_name(self_p->name_p);

    chan_list_init(&list, &elements[0], membersof(elements));
    chan_list_add(&list, &self_p->control.in);
    chan_list_add(&list, self_p->transport.in_p);

    while (1) {
        chan_p = chan_list_poll(&list, NULL);

        if (chan_p == &self_p->control.in) {
            res = read_control_message(self_p);
        } else if (chan_p == self_p->transport.in_p) {
            res = read_server_message(self_p);
        } else {
            res = -1;
        }

        if (res != 0) {
            self_p->on_error(self_p, res);
        }
    }
}
