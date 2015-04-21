/**  audio.c
 *
 *   Copyright (C) 2013-2015 Tox project All Rights Reserved.
 *
 *   This file is part of Tox.
 *
 *   Tox is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tox is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tox. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdlib.h>

#include "audio.h"
#include "rtp.h"

#include "../toxcore/logger.h"

static struct JitterBuffer *jbuf_new(uint32_t capacity);
static void jbuf_clear(struct JitterBuffer *q);
static void jbuf_free(struct JitterBuffer *q);
static int jbuf_write(struct JitterBuffer *q, RTPMessage *m);
static RTPMessage *jbuf_read(struct JitterBuffer *q, int32_t *success);

OpusEncoder* create_audio_encoder (int32_t bitrate, int32_t sampling_rate, int32_t channel_count);
bool reconfigure_audio_decoder(ACSession* ac, int32_t sampling_rate, int8_t channels);



ACSession* ac_new(ToxAV* av, uint32_t friend_id, toxav_receive_audio_frame_cb *cb, void *cb_data)
{
    ACSession *ac = calloc(sizeof(ACSession), 1);
    
    if (!ac) {
        LOGGER_WARNING("Allocation failed! Application might misbehave!");
        return NULL;
    }
    
    if (create_recursive_mutex(ac->queue_mutex) != 0) {
        LOGGER_WARNING("Failed to create recursive mutex!");
        free(ac);
        return NULL;
    }
    
    int status;
    ac->decoder = opus_decoder_create(48000, 2, &status );
    
    if ( status != OPUS_OK ) {
        LOGGER_ERROR("Error while starting audio decoder: %s", opus_strerror(status));
        goto BASE_CLEANUP;
    }
    
    if ( !(ac->j_buf = jbuf_new(3)) ) {
        LOGGER_WARNING("Jitter buffer creaton failed!");
        opus_decoder_destroy(ac->decoder);
        goto BASE_CLEANUP;
    }
    
    /* Initialize encoders with default values */
    ac->encoder = create_audio_encoder(48000, 48000, 2);
    if (ac->encoder == NULL)
        goto DECODER_CLEANUP;
    
    ac->last_encoding_bitrate = 48000;
    ac->last_encoding_sampling_rate = 48000;
    ac->last_encoding_channel_count = 2;
    
    ac->last_decoding_channel_count = 2;
    ac->last_decoding_sampling_rate = 48000;
    ac->last_decoder_reconfiguration = 0; /* Make it possible to reconfigure straight away */
    
    /* These need to be set in order to properly
     * do error correction with opus */
    ac->last_packet_frame_duration = 120;
    ac->last_packet_sampling_rate = 48000;
    
    ac->av = av;
    ac->friend_id = friend_id;
    ac->acb.first = cb;
    ac->acb.second = cb_data;
    
    return ac;
    
DECODER_CLEANUP:
    opus_decoder_destroy(ac->decoder);
    jbuf_free(ac->j_buf);
BASE_CLEANUP:
    pthread_mutex_destroy(ac->queue_mutex);
    free(ac);
    return NULL;
}
void ac_kill(ACSession* ac)
{
    if (!ac)
        return;
    
    opus_encoder_destroy(ac->encoder);
    opus_decoder_destroy(ac->decoder);
    jbuf_free(ac->j_buf);
    
    pthread_mutex_destroy(ac->queue_mutex);
    
    LOGGER_DEBUG("Terminated audio handler: %p", ac);
    free(ac);
}
void ac_do(ACSession* ac)
{
    if (!ac)
        return;
    
    /* Enough space for the maximum frame size (120 ms 48 KHz audio) */
    int16_t tmp[5760];
    
    RTPMessage *msg;
    int rc = 0;
    
    pthread_mutex_lock(ac->queue_mutex);
    while ((msg = jbuf_read(ac->j_buf, &rc)) || rc == 2) {
        pthread_mutex_unlock(ac->queue_mutex);
        
        if (rc == 2) {
            LOGGER_DEBUG("OPUS correction");
            rc = opus_decode(ac->decoder, NULL, 0, tmp,
                            (ac->last_packet_sampling_rate * ac->last_packet_frame_duration / 1000) /
                                ac->last_packet_channel_count, 1);
        } else {
            /* Get values from packet and decode. */
            /* NOTE: This didn't work very well
            rc = convert_bw_to_sampling_rate(opus_packet_get_bandwidth(msg->data));
            if (rc != -1) {
                cs->last_packet_sampling_rate = rc;
            } else {
                LOGGER_WARNING("Failed to load packet values!");
                rtp_free_msg(NULL, msg);
                continue;
            }*/
            
            
            /* Pick up sampling rate from packet */
            memcpy(&ac->last_packet_sampling_rate, msg->data, 4);
            ac->last_packet_sampling_rate = ntohl(ac->last_packet_sampling_rate);
            
            ac->last_packet_channel_count = opus_packet_get_nb_channels(msg->data + 4);
            
            /* 
                * NOTE: even though OPUS supports decoding mono frames with stereo decoder and vice versa,
                * it didn't work quite well.
                */
            if (!reconfigure_audio_decoder(ac, ac->last_packet_sampling_rate, ac->last_packet_channel_count)) {
                LOGGER_WARNING("Failed to reconfigure decoder!");
                rtp_free_msg(NULL, msg);
                continue;
            }
            
            rc = opus_decode(ac->decoder, msg->data + 4, msg->length - 4, tmp, 5760, 0);
            rtp_free_msg(NULL, msg);
        }
        
        if (rc < 0) {
            LOGGER_WARNING("Decoding error: %s", opus_strerror(rc));
        } else if (ac->acb.first) {
            ac->last_packet_frame_duration = (rc * 1000) / ac->last_packet_sampling_rate * ac->last_packet_channel_count;
            
            ac->acb.first(ac->av, ac->friend_id, tmp, rc * ac->last_packet_channel_count,
                          ac->last_packet_channel_count, ac->last_packet_sampling_rate, ac->acb.second);
        }
        
        return;
    }
    pthread_mutex_unlock(ac->queue_mutex);
}
int ac_reconfigure_encoder(ACSession* ac, int32_t bitrate, int32_t sampling_rate, uint8_t channels)
{
    if (!ac)
        return;
    
    /* Values are checked in toxav.c */
    if (ac->last_encoding_sampling_rate != sampling_rate || ac->last_encoding_channel_count != channels) {
        OpusEncoder* new_encoder = create_audio_encoder(bitrate, sampling_rate, channels);
        if (new_encoder == NULL)
            return -1;
        
        opus_encoder_destroy(ac->encoder);
        ac->encoder = new_encoder;
    } else if (ac->last_encoding_bitrate == bitrate)
        return 0; /* Nothing changed */
    else {
        int status = opus_encoder_ctl(ac->encoder, OPUS_SET_BITRATE(bitrate));
        
        if ( status != OPUS_OK ) {
            LOGGER_ERROR("Error while setting encoder ctl: %s", opus_strerror(status));
            return -1;
        }
    }

    ac->last_encoding_bitrate = bitrate;
    ac->last_encoding_sampling_rate = sampling_rate;
    ac->last_encoding_channel_count = channels;
    
    LOGGER_DEBUG ("Reconfigured audio encoder br: %d sr: %d cc:%d", bitrate, sampling_rate, channels);
    return 0;
}
/* called from rtp */
void ac_queue_message(void* acp, RTPMessage *msg)
{
    if (!acp || !msg)
        return;
    
    ACSession* ac = acp;

    pthread_mutex_lock(ac->queue_mutex);
    int ret = jbuf_write(ac->j_buf, msg);
    pthread_mutex_unlock(ac->queue_mutex);

    if (ret == -1)
        rtp_free_msg(NULL, msg);
}


/* JITTER BUFFER WORK */
struct JitterBuffer {
    RTPMessage **queue;
    uint32_t     size;
    uint32_t     capacity;
    uint16_t     bottom;
    uint16_t     top;
};

static struct JitterBuffer *jbuf_new(uint32_t capacity)
{
    unsigned int size = 1;

    while (size <= (capacity * 4)) {
        size *= 2;
    }

    struct JitterBuffer *q;

    if ( !(q = calloc(sizeof(struct JitterBuffer), 1)) ) return NULL;

    if (!(q->queue = calloc(sizeof(RTPMessage *), size))) {
        free(q);
        return NULL;
    }

    q->size = size;
    q->capacity = capacity;
    return q;
}
static void jbuf_clear(struct JitterBuffer *q)
{
    for (; q->bottom != q->top; ++q->bottom) {
        if (q->queue[q->bottom % q->size]) {
            rtp_free_msg(NULL, q->queue[q->bottom % q->size]);
            q->queue[q->bottom % q->size] = NULL;
        }
    }
}
static void jbuf_free(struct JitterBuffer *q)
{
    if (!q) return;

    jbuf_clear(q);
    free(q->queue);
    free(q);
}
static int jbuf_write(struct JitterBuffer *q, RTPMessage *m)
{
    uint16_t sequnum = m->header->sequnum;

    unsigned int num = sequnum % q->size;

    if ((uint32_t)(sequnum - q->bottom) > q->size) {
        LOGGER_DEBUG("Clearing filled jitter buffer: %p", q);
        
        jbuf_clear(q);
        q->bottom = sequnum - q->capacity;
        q->queue[num] = m;
        q->top = sequnum + 1;
        return 0;
    }

    if (q->queue[num])
        return -1;

    q->queue[num] = m;

    if ((sequnum - q->bottom) >= (q->top - q->bottom))
        q->top = sequnum + 1;

    return 0;
}
static RTPMessage *jbuf_read(struct JitterBuffer *q, int32_t *success)
{
    if (q->top == q->bottom) {
        *success = 0;
        return NULL;
    }

    unsigned int num = q->bottom % q->size;

    if (q->queue[num]) {
        RTPMessage *ret = q->queue[num];
        q->queue[num] = NULL;
        ++q->bottom;
        *success = 1;
        return ret;
    }

    if ((uint32_t)(q->top - q->bottom) > q->capacity) {
        ++q->bottom;
        *success = 2;
        return NULL;
    }

    *success = 0;
    return NULL;
}
OpusEncoder* create_audio_encoder (int32_t bitrate, int32_t sampling_rate, int32_t channel_count)
{
    int status = OPUS_OK;
    OpusEncoder* rc = opus_encoder_create(sampling_rate, channel_count, OPUS_APPLICATION_AUDIO, &status);
    
    if ( status != OPUS_OK ) {
        LOGGER_ERROR("Error while starting audio encoder: %s", opus_strerror(status));
        return NULL;
    }
    
    status = opus_encoder_ctl(rc, OPUS_SET_BITRATE(bitrate));
    
    if ( status != OPUS_OK ) {
        LOGGER_ERROR("Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }
    
    status = opus_encoder_ctl(rc, OPUS_SET_COMPLEXITY(10));
    
    if ( status != OPUS_OK ) {
        LOGGER_ERROR("Error while setting encoder ctl: %s", opus_strerror(status));
        goto FAILURE;
    }
    
    return rc;
    
FAILURE:
    opus_encoder_destroy(rc);
    return NULL;
}
bool reconfigure_audio_decoder(ACSession* ac, int32_t sampling_rate, int8_t channels)
{
    if (sampling_rate != ac->last_decoding_sampling_rate || channels != ac->last_decoding_channel_count) {
        if (current_time_monotonic() - ac->last_decoder_reconfiguration < 500)
            return false;
        
        int status;
        OpusDecoder* new_dec = opus_decoder_create(sampling_rate, channels, &status );
        if ( status != OPUS_OK ) {
            LOGGER_ERROR("Error while starting audio decoder(%d %d): %s", sampling_rate, channels, opus_strerror(status));
            return false;
        }
        
        ac->last_decoding_sampling_rate = sampling_rate;
        ac->last_decoding_channel_count = channels;
        ac->last_decoder_reconfiguration = current_time_monotonic();
        
        opus_decoder_destroy(ac->decoder);
        ac->decoder = new_dec;
        
        LOGGER_DEBUG("Reconfigured audio decoder sr: %d cc: %d", sampling_rate, channels);
    }
    
    return true;
}