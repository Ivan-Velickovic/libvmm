/*
 * Copyright 2024, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <libvmm/virq.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/sound.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/mmio.h>
#include <libvmm/virtio/virtq.h>
#include <sddf/sound/queue.h>

// #define DEBUG_SOUND

#if defined(DEBUG_SOUND)
#define LOG_SOUND(...) do{ printf("VIRTIO(SOUND): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_SOUND(...) do{}while(0)
#endif

#define LOG_SOUND_ERR(...) do{ printf("VIRTIO(SOUND)|ERROR: "); printf(__VA_ARGS__); }while(0)

#define CONTROLQ 0
#define EVENTQ 1
#define TXQ 2
#define RXQ 3

static inline struct virtio_snd_device *device_state(struct virtio_device *dev)
{
    return (struct virtio_snd_device *)dev->device_data;
}

static void virtio_snd_mmio_reset(struct virtio_device *dev)
{
    LOG_SOUND("Resetting virtIO sound device\n");

    for (int i = 0; i < VIRTIO_SND_NUM_VIRTQ; i++) {
        dev->vqs[i].ready = false;
        dev->vqs[i].last_idx = 0;
    }
}

static bool virtio_snd_mmio_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    switch (dev->data.DeviceFeaturesSel) {
    case 0:
        // virtIO sound does not define any features
        *features = 0;
        break;
    case 1:
        *features = BIT_HIGH(VIRTIO_F_VERSION_1);
        break;
    default:
        LOG_SOUND_ERR("driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DeviceFeaturesSel);
        return false;
    }

    return true;
}

static bool virtio_snd_mmio_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    bool success = false;
    switch (dev->data.DriverFeaturesSel) {
    // feature bits 0 to 31
    case 0:
        success = (features == 0);
        break;
    // features bits 32 to 63
    case 1:
        success = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
        break;
    default:
        LOG_SOUND_ERR("driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DriverFeaturesSel);
        return false;
    }

    if (success) {
        dev->data.features_happy = 1;
    }

    return success;
}

static bool virtio_snd_mmio_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *ret_val)
{
    struct virtio_snd_device *state = device_state(dev);
    uintptr_t config_base_addr = (uintptr_t)&state->config;
    uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
    uint32_t *config_field_addr = (uint32_t *)(config_base_addr + config_field_offset);
    *ret_val = *config_field_addr;

    return true;
}

static bool virtio_snd_mmio_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t val)
{
    LOG_SOUND_ERR("Not allowed to change sound config.");
    return false;
}

static const char *code_to_str(uint32_t code)
{
    switch (code) {
    case VIRTIO_SND_R_JACK_INFO:
        return "VIRTIO_SND_R_JACK_INFO";
    case VIRTIO_SND_R_JACK_REMAP:
        return "VIRTIO_SND_R_JACK_REMAP";
    case VIRTIO_SND_R_PCM_INFO:
        return "VIRTIO_SND_R_PCM_INFO";
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        return "VIRTIO_SND_R_PCM_SET_PARAMS";
    case VIRTIO_SND_R_PCM_PREPARE:
        return "VIRTIO_SND_R_PCM_PREPARE";
    case VIRTIO_SND_R_PCM_RELEASE:
        return "VIRTIO_SND_R_PCM_RELEASE";
    case VIRTIO_SND_R_PCM_START:
        return "VIRTIO_SND_R_PCM_START";
    case VIRTIO_SND_R_PCM_STOP:
        return "VIRTIO_SND_R_PCM_STOP";
    case VIRTIO_SND_R_CHMAP_INFO:
        return "VIRTIO_SND_R_CHMAP_INFO";
    case VIRTIO_SND_EVT_JACK_CONNECTED:
        return "VIRTIO_SND_EVT_JACK_CONNECTED";
    case VIRTIO_SND_EVT_JACK_DISCONNECTED:
        return "VIRTIO_SND_EVT_JACK_DISCONNECTED";
    case VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED:
        return "VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED";
    case VIRTIO_SND_EVT_PCM_XRUN:
        return "VIRTIO_SND_EVT_PCM_XRUN";
    case VIRTIO_SOUND_S_OK:
        return "VIRTIO_SOUND_S_OK";
    case VIRTIO_SOUND_S_BAD_MSG:
        return "VIRTIO_SOUND_S_BAD_MSG";
    case VIRTIO_SOUND_S_NOT_SUPP:
        return "VIRTIO_SOUND_S_NOT_SUPP";
    case VIRTIO_SOUND_S_IO_ERR:
        return "VIRTIO_SOUND_S_IO_ERR";
    default:
        return "<unknown>";
    }
}

static void virtio_snd_respond(struct virtio_device *dev)
{
    dev->data.InterruptStatus = BIT_LOW(0);
    bool success = virq_inject(dev->virq);
    assert(success);
}

static inline void convert_flag(uint64_t *dest, uint64_t dest_bit, uint64_t src, uint32_t src_bit)
{
    if (src & (1 << src_bit)) {
        *dest |= (1 << dest_bit);
    }
}

static uint64_t virtio_formats_from_sddf(uint64_t formats)
{
    uint64_t result = 0;

    // Currently the enums are identical, but explicitly converting
    // allows us to change the enum values in the future.
    convert_flag(&result, VIRTIO_SND_PCM_FMT_IMA_ADPCM, formats, SOUND_PCM_FMT_IMA_ADPCM);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_MU_LAW,    formats, SOUND_PCM_FMT_MU_LAW);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_A_LAW,     formats, SOUND_PCM_FMT_A_LAW);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S8,        formats, SOUND_PCM_FMT_S8);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U8,        formats, SOUND_PCM_FMT_U8);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S16,       formats, SOUND_PCM_FMT_S16);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U16,       formats, SOUND_PCM_FMT_U16);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S18_3,     formats, SOUND_PCM_FMT_S18_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U18_3,     formats, SOUND_PCM_FMT_U18_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S20_3,     formats, SOUND_PCM_FMT_S20_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U20_3,     formats, SOUND_PCM_FMT_U20_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S24_3,     formats, SOUND_PCM_FMT_S24_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U24_3,     formats, SOUND_PCM_FMT_U24_3);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S20,       formats, SOUND_PCM_FMT_S20);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U20,       formats, SOUND_PCM_FMT_U20);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S24,       formats, SOUND_PCM_FMT_S24);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U24,       formats, SOUND_PCM_FMT_U24);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_S32,       formats, SOUND_PCM_FMT_S32);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_U32,       formats, SOUND_PCM_FMT_U32);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_FLOAT,     formats, SOUND_PCM_FMT_FLOAT);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_FLOAT64,   formats, SOUND_PCM_FMT_FLOAT64);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_DSD_U8,    formats, SOUND_PCM_FMT_DSD_U8);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_DSD_U16,   formats, SOUND_PCM_FMT_DSD_U16);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_DSD_U32,   formats, SOUND_PCM_FMT_DSD_U32);
    convert_flag(&result, VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME, formats, SOUND_PCM_FMT_IEC958_SUBFRAME);

    return result;
}

static uint64_t virtio_rates_from_sddf(uint64_t rates)
{
    uint64_t result = 0;

    convert_flag(&result, VIRTIO_SND_PCM_RATE_5512, rates, SOUND_PCM_RATE_5512);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_8000, rates, SOUND_PCM_RATE_8000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_11025, rates, SOUND_PCM_RATE_11025);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_16000, rates, SOUND_PCM_RATE_16000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_22050, rates, SOUND_PCM_RATE_22050);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_32000, rates, SOUND_PCM_RATE_32000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_44100, rates, SOUND_PCM_RATE_44100);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_48000, rates, SOUND_PCM_RATE_48000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_64000, rates, SOUND_PCM_RATE_64000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_88200, rates, SOUND_PCM_RATE_88200);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_96000, rates, SOUND_PCM_RATE_96000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_176400, rates, SOUND_PCM_RATE_176400);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_192000, rates, SOUND_PCM_RATE_192000);
    convert_flag(&result, VIRTIO_SND_PCM_RATE_384000, rates, SOUND_PCM_RATE_384000);

    return result;
}

static uint8_t virtio_direction_from_sddf(uint8_t direction)
{
    switch (direction) {
    case SOUND_D_INPUT:
        return VIRTIO_SND_D_INPUT;
    case SOUND_D_OUTPUT:
        return VIRTIO_SND_D_OUTPUT;
    }
    LOG_SOUND_ERR("Unknown direction %u\n", direction);
    return (uint8_t) -1;
}

static uint32_t virtio_status_from_sddf(sound_status_t status)
{
    switch (status) {
    case SOUND_S_OK:
        return VIRTIO_SOUND_S_OK;
    case SOUND_S_BAD_MSG:
        return VIRTIO_SOUND_S_BAD_MSG;
    case SOUND_S_NOT_SUPP:
        return VIRTIO_SOUND_S_NOT_SUPP;
    case SOUND_S_IO_ERR:
        return VIRTIO_SOUND_S_IO_ERR;
    case SOUND_S_BUSY:
        return VIRTIO_SOUND_S_IO_ERR;
    }
    return (uint32_t) -1;
}

static void get_pcm_info(struct virtio_snd_pcm_info *dest, const sound_pcm_info_t *src)
{
    dest->features = 0;
    dest->formats = virtio_formats_from_sddf(src->formats);
    dest->rates = virtio_rates_from_sddf(src->rates);
    dest->direction = virtio_direction_from_sddf(src->direction);
    dest->channels_min = src->channels_min;
    dest->channels_max = src->channels_max;
}

static int handle_pcm_info(struct virtio_device *dev,
                           struct virtq *virtq,
                           struct virtio_snd_query_info *query_info,
                           struct virtio_snd_pcm_info *responses,
                           uint32_t response_count,
                           uint32_t *bytes_written)
{
    if (response_count < query_info->count) {
        LOG_SOUND_ERR("Control message response descriptor too small (%d < %d)\n",
                      response_count, query_info->count);
        return -SOUND_S_BAD_MSG;
    }

    struct virtio_snd_device *state = device_state(dev);
    sound_shared_state_t *shared_state = state->shared_state;

    memset(responses, 0, sizeof(*responses) * response_count);

    int i;
    for (i = 0; i < response_count && i < query_info->count; i++) {

        int stream = i + query_info->start_id;
        sound_pcm_info_t *pcm_info = &shared_state->stream_info[stream];

        responses[i].hdr.hda_fn_nid = stream;
        get_pcm_info(&responses[i], pcm_info);
    }

    return i;
}

static int handle_pcm_set_params(struct virtio_device *dev,
                                 uint16_t desc_head,
                                 struct virtio_snd_pcm_set_params *set_params)
{
    struct virtio_snd_device *state = device_state(dev);

    uint32_t cookie;
    int err = ialloc_alloc(&state->free_requests, &cookie);
    if (err < 0) {
        LOG_SOUND_ERR("Failed to allocate cookie\n");
        return -SOUND_S_IO_ERR;
    }

    virtio_snd_request_t *req = &state->requests[cookie];
    req->desc_head = desc_head;
    req->ref_count = 1;
    req->status = SOUND_S_OK;
    req->virtq_idx = CONTROLQ;
    req->bytes_received = 0;

    sound_cmd_t cmd;
    cmd.code = SOUND_CMD_TAKE;
    cmd.cookie = cookie;
    cmd.stream_id = set_params->hdr.stream_id;
    cmd.set_params.channels = set_params->channels;
    cmd.set_params.format = set_params->format;
    cmd.set_params.rate = set_params->rate;

    if (sound_enqueue_cmd(&device_state(dev)->cmd_req, &cmd) != 0) {
        LOG_SOUND_ERR("Failed to enqueue command\n");
        ialloc_free(&state->free_requests, cookie);
        return -SOUND_S_IO_ERR;
    }

    return 0;
}

static int handle_basic_cmd(struct virtio_device *dev,
                            uint16_t desc_head,
                            uint32_t stream_id,
                            sound_cmd_code_t code)
{
    struct virtio_snd_device *state = device_state(dev);

    uint32_t cookie;
    int err = ialloc_alloc(&state->free_requests, &cookie);
    if (err < 0) {
        LOG_SOUND_ERR("Failed to allocate cookie\n");
        return -SOUND_S_IO_ERR;
    }

    virtio_snd_request_t *req = &state->requests[cookie];
    req->desc_head = desc_head;
    req->ref_count = 1;
    req->status = SOUND_S_OK;
    req->virtq_idx = CONTROLQ;
    req->bytes_received = 0;

    sound_cmd_t cmd;
    cmd.code = code;
    cmd.cookie = cookie;
    cmd.stream_id = stream_id;

    if (sound_enqueue_cmd(&device_state(dev)->cmd_req, &cmd) != 0) {
        LOG_SOUND_ERR("Failed to enqueue command\n");
        ialloc_free(&state->free_requests, cookie);
        return -SOUND_S_IO_ERR;
    }

    return 0;
}

static void virtq_enqueue_used(struct virtq *virtq, uint32_t desc_head, uint32_t bytes_written)
{
    struct virtq_used_elem *used_elem = &virtq->used->ring[virtq->used->idx % virtq->num];
    used_elem->id = desc_head;
    used_elem->len = bytes_written;
    virtq->used->idx++;
}

// Returns number of bytes written to virtq
static void handle_control_msg(struct virtio_device *dev,
                               struct virtq *virtq,
                               uint16_t desc_head,
                               bool *notify_driver,
                               bool *respond)
{
    struct virtq_desc *req_desc = &virtq->desc[desc_head];
    struct virtio_snd_hdr *hdr = (void *)req_desc->addr;
    struct virtio_snd_pcm_hdr *pcm_hdr = (void *)hdr;

    bool immediate = false;

    uint32_t bytes_written = 0;

    if ((req_desc->flags & VIRTQ_DESC_F_NEXT) == 0) {
        LOG_SOUND_ERR("Control message missing status descriptor\n");
        return;
    }

    struct virtq_desc *status_desc = &virtq->desc[req_desc->next];
    uint32_t *status_ptr = (void *)status_desc->addr;

    int result;

    switch (hdr->code) {
    case VIRTIO_SND_R_PCM_INFO: {

        if ((status_desc->flags & VIRTQ_DESC_F_NEXT) == 0) {
            LOG_SOUND_ERR("Control message missing response descriptor\n");
            result = -VIRTIO_SOUND_S_BAD_MSG;
            break;
        }

        struct virtq_desc *response_desc = &virtq->desc[status_desc->next];
        result = handle_pcm_info(dev, virtq,
                                 (void *)hdr,
                                 (void *)response_desc->addr,
                                 response_desc->len / sizeof(struct virtio_snd_pcm_info),
                                 &bytes_written);
        if (result >= 0) {
            bytes_written += result * sizeof(struct virtio_snd_pcm_info);
        }
        immediate = true;
        break;
    }
    case VIRTIO_SND_R_PCM_SET_PARAMS:
        result = handle_pcm_set_params(dev, desc_head, (void *)hdr);
        break;
    case VIRTIO_SND_R_PCM_PREPARE:
        result = handle_basic_cmd(dev, desc_head, pcm_hdr->stream_id, SOUND_CMD_PREPARE);
        break;
    case VIRTIO_SND_R_PCM_RELEASE:
        result = handle_basic_cmd(dev, desc_head, pcm_hdr->stream_id, SOUND_CMD_RELEASE);
        break;
    case VIRTIO_SND_R_PCM_START:
        result = handle_basic_cmd(dev, desc_head, pcm_hdr->stream_id, SOUND_CMD_START);
        break;
    case VIRTIO_SND_R_PCM_STOP:
        result = handle_basic_cmd(dev, desc_head, pcm_hdr->stream_id, SOUND_CMD_STOP);
        break;
    case VIRTIO_SND_R_JACK_INFO:
    case VIRTIO_SND_R_JACK_REMAP:
    case VIRTIO_SND_R_CHMAP_INFO:
        LOG_SOUND_ERR("Control message not implemented: %s\n", code_to_str(hdr->code));
        result = -VIRTIO_SOUND_S_NOT_SUPP;
        break;
    default:
        LOG_SOUND_ERR("Unknown control message %s\n", code_to_str(hdr->code));
        result = -VIRTIO_SOUND_S_BAD_MSG;
        break;
    }

    uint32_t status = VIRTIO_SOUND_S_OK;
    if (result < 0) {
        LOG_SOUND_ERR("Command failed with result %d\n", -result);
        immediate = true;
        status = -result;
    }

    if (immediate) {
        *status_ptr = status;
        bytes_written += sizeof(uint32_t);
        virtq_enqueue_used(virtq, desc_head, bytes_written);
    } else {
        *notify_driver = true;
        assert(bytes_written == 0);
    }
    *respond = immediate;
}

static bool perform_xfer(struct virtio_device *dev,
                         struct virtq *virtq,
                         struct virtq_desc *desc,
                         bool transmit,
                         int stream_id,
                         int cookie,
                         int *sent)
{
    struct virtio_snd_device *state = device_state(dev);

    uintptr_t buf_offset;
    if (!queue_dequeue_front(&state->free_buffers, &buf_offset)) {
        LOG_SOUND_ERR("No free buffers\n");
        return false;
    }

    uint32_t pcm_transmitted = 0;
    uint32_t pcm_remaining = SOUND_PCM_BUFFER_SIZE;

    // Decompose descriptor chain into one or more sDDF requests.
    for (;
         desc->flags & VIRTQ_DESC_F_NEXT;
         desc = &virtq->desc[desc->next]) {
        if (!!(desc->flags & VIRTQ_DESC_F_WRITE) == transmit) {
            LOG_SOUND_ERR("Incorrect xfer buffer type\n");
            return false;
        }

        uint32_t desc_transmitted = 0;
        uint32_t desc_remaining = desc->len;

        while (desc_remaining > 0) {

            int to_xfer = MIN(desc_remaining, pcm_remaining);

            if (transmit) {
                void *pcm_buffer = state->data_region + buf_offset;
                memcpy(pcm_buffer + pcm_transmitted,
                       (void *)desc->addr + desc_transmitted, to_xfer);
            }
            desc_transmitted += to_xfer;
            desc_remaining -= to_xfer;

            pcm_transmitted += to_xfer;
            pcm_remaining -= to_xfer;

            // If current to-be-sent request is full, send it.
            if (pcm_remaining == 0) {

                sound_pcm_t pcm;
                pcm.io_or_offset = buf_offset;
                pcm.len = pcm_transmitted;
                pcm.stream_id = stream_id;
                pcm.cookie = cookie;
                pcm.status = 0;
                pcm.latency_bytes = 0;

                if (sound_enqueue_pcm(&state->pcm_req, &pcm) != 0) {
                    LOG_SOUND_ERR("Failed to enqueue to pcm request\n");
                    return false;
                }

                (*sent)++;

                if (!queue_dequeue_front(&state->free_buffers, &buf_offset)) {
                    LOG_SOUND_ERR("No free buffers\n");
                    return false;
                }

                pcm_remaining = SOUND_PCM_BUFFER_SIZE;
                pcm_transmitted = 0;
            }
        }
    }

    // Transmit remaining PCM data.
    if (pcm_transmitted > 0) {
        sound_pcm_t pcm;
        pcm.io_or_offset = buf_offset;
        pcm.len = pcm_transmitted;
        pcm.stream_id = stream_id;
        pcm.cookie = cookie;
        pcm.status = 0;
        pcm.latency_bytes = 0;

        if (sound_enqueue_pcm(&state->pcm_req, &pcm) != 0) {
            LOG_SOUND_ERR("Failed to enqueue to tx used\n");
            return false;
        }
        (*sent)++;
    }
    return true;
}

static void handle_xfer(struct virtio_device *dev,
                        struct virtq *virtq,
                        uint16_t desc_head,
                        bool transmit,
                        bool *notify_driver, bool *respond)
{
    struct virtq_desc *req_desc = &virtq->desc[desc_head];
    struct virtio_snd_pcm_xfer *hdr = (void *)req_desc->addr;

    struct virtio_snd_device *state = device_state(dev);

    if ((req_desc->flags & VIRTQ_DESC_F_NEXT) == 0) {
        LOG_SOUND_ERR("XFER message missing data\n");
        return;
    }

    uint32_t cookie;
    int err = ialloc_alloc(&state->free_requests, &cookie);
    if (err < 0) {
        LOG_SOUND_ERR("Failed to allocate cookie\n");
        return;
    }

    struct virtq_desc *desc = &virtq->desc[req_desc->next];
    int sent = 0;
    bool success = perform_xfer(dev, virtq, desc, transmit,
                                hdr->stream_id, cookie, &sent);

    if (sent == 0) {
        // If we sent zero, respond immediately.
        for (;
             desc->flags & VIRTQ_DESC_F_NEXT;
             desc = &virtq->desc[desc->next]);

        assert(desc->flags & VIRTQ_DESC_F_WRITE);

        uint32_t *status_ptr = (void *)desc->addr;
        *status_ptr = VIRTIO_SOUND_S_IO_ERR;

        virtq_enqueue_used(virtq, desc_head, sizeof(uint32_t));
        ialloc_free(&state->free_requests, cookie);

        *respond = true;
        return;
    }

    virtio_snd_request_t *req = &state->requests[cookie];
    req->desc_head = desc_head;
    req->ref_count = sent;
    req->status = SOUND_S_OK;
    req->virtq_idx = transmit ? TXQ : RXQ;
    req->bytes_received = 0;
    if (!success) {
        req->status = VIRTIO_SOUND_S_IO_ERR;
    }

    *notify_driver = true;
}

static void handle_virtq(struct virtio_device *dev,
                         int index, bool *notify_driver, bool *respond)
{
    virtio_queue_handler_t *vq = &dev->vqs[index];
    struct virtq *virtq = &vq->virtq;

    uint16_t idx = vq->last_idx;
    for (; idx != virtq->avail->idx; idx++) {

        uint16_t desc_head = virtq->avail->ring[idx % virtq->num];

        switch (index) {
        case CONTROLQ:
            handle_control_msg(dev, virtq, desc_head, notify_driver, respond);
            break;
        case TXQ:
            handle_xfer(dev, virtq, desc_head, true, notify_driver, respond);
            break;
        case RXQ:
            handle_xfer(dev, virtq, desc_head, false, notify_driver, respond);
            break;
        default:
            LOG_SOUND_ERR("Queue %d not implemented", index);
        }
    }
    vq->last_idx = idx;
}

static bool virtio_snd_mmio_queue_notify(struct virtio_device *dev)
{
    struct virtio_snd_device *state = device_state(dev);

    if (dev->data.QueueSel > VIRTIO_SND_NUM_VIRTQ) {
        LOG_SOUND_ERR("Invalid queue\n");
        return false;
    }

    bool notify_driver = false;
    bool respond = false;

    handle_virtq(dev, dev->data.QueueNotify, &notify_driver, &respond);

    if (notify_driver) {
        microkit_notify(state->server_ch);
    }
    if (respond) {
        virtio_snd_respond(dev);
    }

    return true;
}

static virtio_device_funs_t functions = {
    .device_reset = virtio_snd_mmio_reset,
    .get_device_features = virtio_snd_mmio_get_device_features,
    .set_driver_features = virtio_snd_mmio_set_driver_features,
    .get_device_config = virtio_snd_mmio_get_device_config,
    .set_device_config = virtio_snd_mmio_set_device_config,
    .queue_notify = virtio_snd_mmio_queue_notify,
};

bool virtio_mmio_snd_init(struct virtio_snd_device *sound_dev,
                          uintptr_t region_base,
                          uintptr_t region_size,
                          size_t virq,
                          sound_shared_state_t *shared_state,
                          sound_queues_t *queues,
                          uintptr_t data_region,
                          int server_ch)
{
    struct virtio_device *dev = &sound_dev->virtio_device;

    dev->data.DeviceID = DEVICE_ID_VIRTIO_SOUND;
    dev->data.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->funs = &functions;
    dev->vqs = sound_dev->vqs;
    dev->num_vqs = VIRTIO_SND_NUM_VIRTQ;
    dev->virq = virq;
    dev->device_data = sound_dev;

    sound_dev->config.jacks = 0;
    sound_dev->config.streams = shared_state->streams;
    sound_dev->config.chmaps = 0;

    ialloc_init(&sound_dev->free_requests,
                sound_dev->free_requests_data,
                VIRTIO_SND_MAX_REQUESTS);

    queue_init(&sound_dev->free_buffers,
               sizeof(uintptr_t),
               sound_dev->free_buffers_data,
               SOUND_PCM_QUEUE_SIZE);

    sound_dev->shared_state = shared_state;
    sound_dev->cmd_req = queues->cmd_req;
    sound_dev->cmd_res = queues->cmd_res;
    sound_dev->pcm_req = queues->pcm_req;
    sound_dev->pcm_res = queues->pcm_res;
    sound_dev->data_region = (void *)data_region;
    sound_dev->server_ch = server_ch;

    for (uintptr_t i = 0; i < sound_dev->pcm_req.size; i++) {
        uintptr_t offset = i * SOUND_PCM_BUFFER_SIZE;
        queue_enqueue(&sound_dev->free_buffers, &offset);
    }

    return virtio_mmio_register_device(dev, region_base, region_size, virq);
}

static unsigned copy_rx_data(struct virtq *virtq,
                             struct virtq_desc *desc,
                             virtio_snd_request_t *req,
                             void *pcm, unsigned pcm_len)
{
    uint32_t desc_position = 0;

    for (;
         desc->flags & VIRTQ_DESC_F_NEXT;
         desc = &virtq->desc[desc->next]) {
        if ((desc->flags & VIRTQ_DESC_F_WRITE) == 0) {
            LOG_SOUND_ERR("Expected VIRTQ_DESC_F_WRITE on RX buffer\n");
            req->status = SOUND_S_BAD_MSG;
            continue;
        }

        if (pcm_len == 0) {
            break;
        }

        if (desc_position + desc->len > req->bytes_received) {
            assert(desc_position <= req->bytes_received);

            uint32_t offset = req->bytes_received - desc_position;
            uint32_t to_write = MIN(pcm_len, desc->len - offset);

            memcpy((void *)desc->addr + offset, pcm, to_write);
            pcm += to_write;
            req->bytes_received += to_write;
            pcm_len -= to_write;
        }

        desc_position += desc->len;
    }

    // Sanity check when RX request is complete.
    if (req->ref_count == 0) {
        if (pcm_len != 0) {
            LOG_SOUND_ERR("Received too much PCM for RX\n");
            req->status = SOUND_S_BAD_MSG;
        }
        if (desc_position != req->bytes_received) {
            LOG_SOUND_ERR(
                "Did not receive enough PCM for RX: desc_position %u, bytes_received %u\n",
                desc_position, req->bytes_received);

            req->status = SOUND_S_BAD_MSG;
        }
        if (desc->flags & VIRTQ_DESC_F_NEXT) {
            LOG_SOUND_ERR("Desc not fully advanced\n");
            req->status = SOUND_S_BAD_MSG;
        }
        if ((desc->flags & VIRTQ_DESC_F_WRITE) == 0) {
            LOG_SOUND_ERR("Expected VIRTQ_DESC_F_WRITE on status buffer\n");
            req->status = SOUND_S_BAD_MSG;
        }
    }
    return desc_position;
}

static bool respond_to_request(virtio_snd_request_t *req,
                               struct virtio_device *dev,
                               void *pcm, unsigned pcm_len,
                               void *response, unsigned response_len)
{
    if (req->ref_count == 0) {
        LOG_SOUND_ERR("Message ref count is 0 (too many responses)\n");
        return false;
    }

    uint16_t desc_head = req->desc_head;

    assert(req->virtq_idx < VIRTIO_SND_NUM_VIRTQ);
    struct virtq *virtq = &dev->vqs[req->virtq_idx].virtq;

    struct virtq_desc *req_desc = &virtq->desc[desc_head];
    struct virtq_desc *res_desc = &virtq->desc[req_desc->next];

    unsigned used;
    if (req->virtq_idx == RXQ) {
        used = copy_rx_data(virtq, res_desc, req, pcm, pcm_len);
    } else {
        used = 0;
    }

    if (--req->ref_count > 0) {
        return false;
    }

    struct virtq_desc *status_desc = res_desc;
    for (;
         status_desc->flags & VIRTQ_DESC_F_NEXT;
         status_desc = &virtq->desc[status_desc->next]);

    if (status_desc == req_desc || (status_desc->flags & VIRTQ_DESC_F_WRITE) == 0) {
        LOG_SOUND_ERR("Message must contain writeable status descriptor\n");
    } else {
        memcpy((void *)status_desc->addr, response, response_len);
        used += response_len;
    }

    virtq_enqueue_used(virtq, desc_head, used);

    return true;
}

void virtio_snd_notified(struct virtio_snd_device *state)
{
    struct virtio_device *dev = &state->virtio_device;
    bool respond = false;

    sound_cmd_t cmd;
    while (sound_dequeue_cmd(&state->cmd_res, &cmd) == 0) {

        virtio_snd_request_t *req = &state->requests[cmd.cookie];
        if (cmd.status != SOUND_S_OK) {
            req->status = cmd.status;
        }

        uint32_t response = virtio_status_from_sddf(req->status);

        bool responded = respond_to_request(req, dev, NULL, 0,
                                            &response, sizeof(response));
        if (responded) {
            ialloc_free(&state->free_requests, cmd.cookie);
            respond = true;
        }
    }

    sound_pcm_t pcm;
    while (sound_dequeue_pcm(&state->pcm_res, &pcm) == 0) {

        virtio_snd_request_t *req = &state->requests[pcm.cookie];
        if (pcm.status != SOUND_S_OK) {
            req->status = pcm.status;
        }

        struct virtio_snd_pcm_status response;
        response.status = virtio_status_from_sddf(req->status);
        response.latency_bytes = pcm.latency_bytes;

        void *pcm_buffer = state->data_region + pcm.io_or_offset;
        bool responded = respond_to_request(req, dev,
                                            pcm_buffer, pcm.len,
                                            &response, sizeof(response));
        if (responded) {
            ialloc_free(&state->free_requests, pcm.cookie);
            respond = true;
        }

        uintptr_t buf_offset = (uintptr_t)pcm.io_or_offset;
        queue_enqueue(&state->free_buffers, &buf_offset);
    }

    if (respond) {
        virtio_snd_respond(dev);
    }
}
