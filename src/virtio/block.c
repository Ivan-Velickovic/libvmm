#include "virtio/config.h"
#include "virtio/virtq.h"
#include "virtio/mmio.h"
#include "virtio/block.h"
#include "util/util.h"
#include "virq.h"
#include "block/libblocksharedringbuffer/include/sddf_blk_shared_ringbuffer.h"

/* Uncomment this to enable debug logging */
#define DEBUG_BLOCK

#if defined(DEBUG_BLOCK)
#define LOG_BLOCK(...) do{ printf("VIRTIO(BLOCK): "); printf(__VA_ARGS__); }while(0)
#else
#define LOG_BLOCK(...) do{}while(0)
#endif

#define LOG_BLOCK_ERR(...) do{ printf("VIRTIO(BLOCK)|ERROR: "); printf(__VA_ARGS__); }while(0)

// @ivanv: put in util or remove
#define BIT_LOW(n)  (1ul<<(n))
#define BIT_HIGH(n) (1ul<<(n - 32 ))
// @ericc: maybe put this into util.c?
/* Returns uint32_t where all bits above and including position is set to 1 */
#define MASK_ABOVE_POSITION_INCLUSIVE(position) (~(((uint32_t)1 << (position)) - 1))
/* Returns uint32_t where all bits below position is set to 1 */
#define MASK_BELOW_POSITION_EXCLUSIVE(position) (((uint32_t)1 << (position)) - 1)

/* Data region bookkeeping */
/* Number of bits in an element of available bitmap */
#define DATA_REGION_AVAIL_BITMAP_ELEM_SIZE 32
/* Size of available bitmap */
#define DATA_REGION_AVAIL_BITMAP_SIZE (SDDF_BLK_NUM_DATA_BUFFERS / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE)

// @ericc: Maybe move this into virtio.c, and store a pointer in virtio_device struct?
static struct virtio_blk_config blk_config;

/* Mapping for command ID and its virtio descriptor */
static struct virtio_blk_cmd_store {
    uint16_t sent_cmds[SDDF_BLK_NUM_DATA_BUFFERS]; /* index is command ID, maps to virtio descriptor head */
    uint32_t freelist[SDDF_BLK_NUM_DATA_BUFFERS]; /* index is free command ID, maps to next free command ID */
    uint32_t head;
    uint32_t tail;
    uint32_t num_free;
} cmd_store;

/* Data struct that handles allocation and freeing of data buffers in sDDF shared memory region */
static struct data_region {
    uint32_t avail_bitpos; /* bit position of next avail buffer */
    uint32_t avail_bitmap[DATA_REGION_AVAIL_BITMAP_SIZE]; /* Bit map representing avail data buffers */
    uint32_t num_buffers; /* number of buffers in data region */
    uintptr_t addr; /* encoded base address of data region */
} data_region;

static void virtio_blk_mmio_reset(struct virtio_device *dev)
{
    dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT].ready = 0;
    dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT].last_idx = 0;
}

static int virtio_blk_mmio_get_device_features(struct virtio_device *dev, uint32_t *features)
{
    if (dev->data.Status & VIRTIO_CONFIG_S_FEATURES_OK) {
        LOG_BLOCK_ERR("driver somehow wants to read device features after FEATURES_OK\n");
    }

    switch (dev->data.DeviceFeaturesSel) {
        // feature bits 0 to 31
        case 0:
            *features = BIT_LOW(VIRTIO_BLK_F_FLUSH);
            break;
        // features bits 32 to 63
        case 1:
            *features = BIT_HIGH(VIRTIO_F_VERSION_1);
            break;
        default:
            LOG_BLOCK_ERR("driver sets DeviceFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DeviceFeaturesSel);
            return 0;
    }
    return 1;
}

static int virtio_blk_mmio_set_driver_features(struct virtio_device *dev, uint32_t features)
{
    // According to virtio initialisation protocol,
    // this should check what device features were set, and return the subset of features understood
    // by the driver. However, for now we ignore what the driver sets, and just return the features we support.
    int success = 1;

    switch (dev->data.DriverFeaturesSel) {
        // feature bits 0 to 31
        case 0:
            success = (features & BIT_LOW(VIRTIO_BLK_F_FLUSH));
            break;
        // features bits 32 to 63
        case 1:
            success = (features == BIT_HIGH(VIRTIO_F_VERSION_1));
            break;
        default:
            LOG_BLOCK_ERR("driver sets DriverFeaturesSel to 0x%x, which doesn't make sense\n", dev->data.DriverFeaturesSel);
            success = 0;
    }

    if (success) {
        dev->data.features_happy = 1;
    }

    return success;
}

static int virtio_blk_mmio_get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *ret_val)
{
    uintptr_t config_base_addr = (uintptr_t)&blk_config;
    uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
    uint32_t *config_field_addr = (uint32_t *)(config_base_addr + config_field_offset);
    *ret_val = *config_field_addr;
    LOG_BLOCK("get device config with base_addr 0x%x and field_address 0x%x has value %d\n", config_base_addr, config_field_addr, *ret_val);
    return 1;
}

static int virtio_blk_mmio_set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t val)
{
    uintptr_t config_base_addr = (uintptr_t)&blk_config;
    uintptr_t config_field_offset = (uintptr_t)(offset - REG_VIRTIO_MMIO_CONFIG);
    uint32_t *config_field_addr = (uint32_t *)(config_base_addr + config_field_offset);
    *config_field_addr = val;
    LOG_BLOCK("set device config with base_addr 0x%x and field_address 0x%x with value %d\n", config_base_addr, config_field_addr, val);
    return 1;
}

static void virtio_blk_used_buffer(struct virtio_device *dev, uint16_t desc)
{
    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT].virtq;
    struct virtq_used_elem used_elem = {desc, 0};

    virtq->used->ring[virtq->used->idx % virtq->num] = used_elem;
    virtq->used->idx++;
}

static void virtio_blk_used_buffer_virq_inject(struct virtio_device *dev)
{
    // set the reason of the irq: used buffer notification to virtio
    dev->data.InterruptStatus = BIT_LOW(0);

    bool success = virq_inject(GUEST_VCPU_ID, dev->virq);
    assert(success);
}

/* Set response to virtio command to error */
static void virtio_blk_set_cmd_fail(struct virtio_device *dev, uint16_t desc)
{
    struct virtq *virtq = &dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT].virtq;

    uint16_t curr_virtio_desc = desc;
    for (;virtq->desc[curr_virtio_desc].flags & VIRTQ_DESC_F_NEXT; curr_virtio_desc = virtq->desc[curr_virtio_desc].next){}
    *((uint8_t *)virtq->desc[curr_virtio_desc].addr) = VIRTIO_BLK_S_IOERR;
}

/**
 * Check if the command store is full.
 * 
 * @return true if command store is full, false otherwise.
 */
static inline bool virtio_blk_cmd_store_full()
{
    return cmd_store.num_free == 0;
}

/**
 * Allocate a command store slot for a new virtio command.
 * 
 * @param desc virtio descriptor to store in a command store slot 
 * @param id pointer to command ID allocated
 * @return 0 on success, -1 on failure
 */
static inline int virtio_blk_cmd_store_allocate(uint16_t desc, uint32_t *id)
{
    if (virtio_blk_cmd_store_full()) {
        return -1;
    }

    // Store descriptor into head of command store
    cmd_store.sent_cmds[cmd_store.head] = desc;
    *id = cmd_store.head;

    // Update head to next free command store slot
    cmd_store.head = cmd_store.freelist[cmd_store.head];

    // Update number of free command store slots
    cmd_store.num_free--;
    
    return 0;
}

/**
 * Retrieve and free a command store slot.
 * 
 * @param id command ID to be retrieved
 * @return virtio descriptor stored in slot
 */
static inline uint16_t virtio_blk_cmd_store_retrieve(uint32_t id)
{
    assert(cmd_store.num_free < SDDF_BLK_NUM_DATA_BUFFERS);

    if (cmd_store.num_free == 0) {
        // Head points to stale index, so restore it
        cmd_store.head = id;
    }

    cmd_store.freelist[cmd_store.tail] = id;
    cmd_store.tail = id;

    cmd_store.num_free++;

    return &cmd_store.sent_cmds[id];
}

/**
 * Convert a bit position to the address of the corresponding data buffer.
 * 
 * @param bitpos bit position of the data buffer
 * @return address of the data buffer
 */
static inline uintptr_t data_region_bitpos_to_addr(uint32_t bitpos)
{
    return data_region.addr + ((uintptr_t)bitpos * SDDF_BLK_DATA_BUFFER_SIZE);
}

/**
 * Convert an address to the bit position of the corresponding data buffer.
 * 
 * @param addr address of the data buffer
 * @return bit position of the data buffer
 */
static inline uint32_t data_region_addr_to_bitpos(uintptr_t addr)
{
    return (uint32_t)((addr - data_region.addr) / SDDF_BLK_DATA_BUFFER_SIZE);
}

/**
 * Check if count number of buffers will overflow the end of the data region.
 * 
 * @param count number of buffers to check
 * @return true if count number of buffers will overflow the end of the data region, false otherwise
 */
static inline bool data_region_overflow(uint16_t count)
{
    return (data_region.avail_bitpos + count > data_region.num_buffers);
}

/**
 * Reset the data region bitpos to the start of the data region.
 * Intended to be called when a command will cause an overflow.
 */
static inline void data_region_loop_over()
{
    data_region.avail_bitpos = 0;
}

/**
 * Check if the data region has count number of free buffers available after current bitpos.
 *
 * @param count number of buffers to check.
 *
 * @return true indicates the data region has count number of free buffers after current bitpos, false otherwise.
 */
static bool data_region_full(uint16_t count)
{
    if (count > data_region.num_buffers) {
        return true;
    }

    if (data_region_overflow(count)) {
        return true;
    }

    // Check for all 0's in the next count many bits
    unsigned int start_bitpos = data_region.avail_bitpos;
    unsigned int end_bitpos = data_region.avail_bitpos + count;
    unsigned int curr_idx = start_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    unsigned int end_idx = end_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    uint32_t mask;
    if (curr_idx != end_idx) {
        // Check the bits in the first index
        mask = MASK_ABOVE_POSITION_INCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        if (data_region.avail_bitmap[curr_idx] & mask != mask) {
            return true;
        }
        curr_idx++;

        // Check the bits in indices between the first and last
        mask = MASK_ABOVE_POSITION_INCLUSIVE(0);
        for (; curr_idx != end_idx; curr_idx++) {
            if (data_region.avail_bitmap[curr_idx] & mask != mask) {
                return true;
            }
        }

        // Check the bits in the last index
        mask = MASK_BELOW_POSITION_EXCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        if (data_region.avail_bitmap[end_idx] & mask != mask) {
            return true;
        }
    } else {
        // Check the bits in the index
        // Create a mask as such 00000000_00001111_11110000_00000000, check whether section in middle is all 1's, if not then its full
        mask = MASK_ABOVE_POSITION_INCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE) & MASK_BELOW_POSITION_EXCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        if (data_region.avail_bitmap[curr_idx] & mask != mask) {
            return true;
        }
    }

    return false;
}

/**
 * Get count many free buffers in the data region.
 *
 * @param addr pointer to base address of the resulting contiguous buffer.
 * @param count number of free buffers to get.
 *
 * @return -1 when data region is full, 0 on success.
 */
static int data_region_get_buffer(uintptr_t *addr, uint16_t count)
{
    if (data_region_full() || data_region_overflow(count)) {
        return -1;
    }

    *addr = data_region_bitpos_to_addr(data_region.avail_bitpos);

    // Set the next count many bits as unavailable
    unsigned int start_bitpos = data_region.avail_bitpos;
    unsigned int end_bitpos = data_region.avail_bitpos + count;
    unsigned int curr_idx = start_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    unsigned int end_idx = end_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    uint32_t mask;
    if (curr_idx != end_idx) {
        // Set the bits in the first index
        mask = MASK_BELOW_POSITION_EXCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[curr_idx] &= mask;
        curr_idx++;

        // Set the bits in indices between the first and last
        mask = MASK_BELOW_POSITION_EXCLUSIVE(0);
        for (; curr_idx != end_idx; curr_idx++) {
            data_region.avail_bitmap[curr_idx] &= mask;
        }

        // Set the bits in the last index
        mask = MASK_ABOVE_POSITION_INCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[end_idx] &= mask;
    } else {
        // Set the bits in the index
        // Create a mask as such 11111111_11110000_00001111_11111111, set some section in middle to be 0
        mask = MASK_BELOW_POSITION_EXCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE) & MASK_ABOVE_POSITION_INCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[curr_idx] &= mask;
    }

    // Update the bitpos
    if (end_bitpos == data_region.num_buffers) {
        end_bitpos = 0;
    }
    data_region.avail_bitpos = end_bitpos;

    return 0;
}

/**
 * Free count many available buffers in the data region.
 *
 * @param addr base address of the contiguous buffer to free.
 * @param count number of buffers to free.
 */
static void data_region_free_buffer(uintptr_t addr, uint16_t count)
{
    unsigned int start_bitpos = data_region_addr_to_bitpos(addr);
    unsigned int end_bitpos = start_bitpos + count;
    unsigned int curr_idx = start_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    unsigned int end_idx = end_bitpos / DATA_REGION_AVAIL_BITMAP_ELEM_SIZE;
    uint32_t mask;

    // Assert here in case we try to free buffers that overflow the data region
    assert(start_bitpos + count <= data_region.num_buffers);

    // Set the next many bits from the bit corresponding with addr as available
    if (curr_idx != end_idx) {
        // Set the bits in the first index
        mask = MASK_ABOVE_POSITION_INCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[curr_idx] |= mask;
        curr_idx++;

        // Set the bits in indices between the first and last
        mask = MASK_ABOVE_POSITION_INCLUSIVE(0);
        for (; curr_idx != end_idx; curr_idx++) {
            data_region.avail_bitmap[curr_idx] |= mask;
        }

        // Set the bits in the last index
        mask = MASK_BELOW_POSITION_EXCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[end_idx] |= mask;
    } else {
        // Set the bits in the index
        // Create a mask as such 00000000_00001111_11110000_00000000, set all bits in the middle section to 1.
        mask = MASK_ABOVE_POSITION_INCLUSIVE(start_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE) & MASK_BELOW_POSITION_EXCLUSIVE(end_bitpos % DATA_REGION_AVAIL_BITMAP_ELEM_SIZE);
        data_region.avail_bitmap[curr_idx] |= mask;
    }

    return 0;
}

static int virtio_blk_mmio_queue_notify(struct virtio_device *dev)
{
    // @ericc: If multiqueue feature bit negotiated, should read which queue has been selected from dev->data->QueueSel,
    // but for now we just assume it's the one and only default queue
    virtio_queue_handler_t *vq = &dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT];
    struct virtq *virtq = &vq->virtq;

    sddf_blk_ring_handle_t *sddf_ring_handle = dev->sddf_ring_handles[SDDF_BLK_DEFAULT_RING];

    bool has_error = false; /* if any command has to be dropped due to any number of reasons (ring buffer full, cmd_store full), this becomes true */
    
    /* get next available command to be handled */
    uint16_t idx = vq->last_idx;

    LOG_BLOCK("------------- Driver notified device -------------\n");
    for (;idx != virtq->avail->idx; idx++) {
        uint16_t desc_head = virtq->avail->ring[idx % virtq->num];

        uint16_t curr_desc_head = desc_head;

        // Print out what the command type is
        struct virtio_blk_outhdr *virtio_cmd = (void *)virtq->desc[curr_desc_head].addr;
        LOG_BLOCK("----- Command type is 0x%x -----\n", virtio_cmd->type);
        
        // Parse different commands
        switch (virtio_cmd->type) {
            // header -> body -> reply
            case VIRTIO_BLK_T_IN: {
                LOG_BLOCK("Command type is VIRTIO_BLK_T_IN\n");
                LOG_BLOCK("Sector (read/write offset) is %d (x512)\n", virtio_cmd->sector);
                
                curr_desc_head = virtq->desc[curr_desc_head].next;
                LOG_BLOCK("Descriptor index is %d, Descriptor flags are: 0x%x, length is 0x%x\n", curr_desc_head, (uint16_t)virtq->desc[curr_desc_head].flags, virtq->desc[curr_desc_head].len);
                
                uintptr_t data = virtq->desc[curr_desc_head].addr;
                uint32_t sddf_count = virtq->desc[curr_desc_head].len / SDDF_BLK_DATA_BUFFER_SIZE;

                // Check if cmd store is full, if data region is full, if cmd ring is full
                // If these all pass then this command can be handled successfully

                // Book keep the command
                
                // Allocate data buffer from data region based on sddf_count
                // Pass this allocated data buffer to sddf read command, then enqueue it

                break;
            }
            case VIRTIO_BLK_T_OUT: {
                LOG_BLOCK("Command type is VIRTIO_BLK_T_OUT\n");
                LOG_BLOCK("Sector (read/write offset) is %d (x512)\n", virtio_cmd->sector);
                
                curr_desc_head = virtq->desc[curr_desc_head].next;
                LOG_BLOCK("Descriptor index is %d, Descriptor flags are: 0x%x, length is 0x%x\n", curr_desc_head, (uint16_t)virtq->desc[curr_desc_head].flags, virtq->desc[curr_desc_head].len);
                
                uintptr_t data = virtq->desc[curr_desc_head].addr;
                uint32_t sddf_count = virtq->desc[curr_desc_head].len / SDDF_BLK_DATA_BUFFER_SIZE;
                
                // Check if cmd store is full, if data region is full, if cmd ring is full
                // If these all pass then this command can be handled successfully

                // Book keep the command
                
                // Allocate data buffer from data region based on sddf_count
                // Copy data from virtio buffer to data buffer, create sddf write command and initialise it with data buffer, then enqueue it

                break;
            }
            case VIRTIO_BLK_T_FLUSH: {
                LOG_BLOCK("Command type is VIRTIO_BLK_T_FLUSH\n");

                // Check if cmd store is full, if cmd ring is full
                // If these all pass then this command can be handled successfully
                // If fail, then set response to virtio command to error

                // Book keep the command

                // Create sddf flush command and enqueue it
                break;
            }
        }
    }

    // Update virtq index to the next available command to be handled
    vq->last_idx = idx;
    
    if (has_error) {
        virtio_blk_used_buffer_virq_inject(dev);
    }
    
    // @ericc: there is a world where all commands to be handled during this batch are dropped and hence this notify to the other PD would be redundant, i guess?
    microkit_notify(dev->sddf_ch);
    
    return 1;
}

void virtio_blk_handle_resp(struct virtio_device *dev) {
    sddf_blk_ring_handle_t *sddf_ring_handle = dev->sddf_ring_handles[SDDF_BLK_DEFAULT_RING];

    sddf_blk_response_status_t sddf_ret_status;
    uint32_t sddf_ret_desc;
    uint16_t sddf_ret_count;
    uint32_t sddf_ret_id;
    while (!sddf_blk_resp_ring_empty(sddf_ring_handle)) {
        sddf_blk_dequeue_resp(sddf_ring_handle, &sddf_ret_status, &sddf_ret_desc, &sddf_ret_count, &sddf_ret_id);
        
        /* Freeing and retrieving command store */
        uint16_t virtio_desc = virtio_blk_cmd_store_retrieve(sddf_ret_id);
        struct virtq *virtq = &dev->vqs[VIRTIO_BLK_VIRTQ_DEFAULT].virtq;
        struct virtio_blk_outhdr *virtio_cmd = (void *)virtq->desc[virtio_desc].addr;

        /* Responding error to virtio if needed */
        if (sddf_ret_status == SDDF_BLK_RESPONSE_ERROR) {
            virtio_blk_set_cmd_fail(dev, virtio_desc);
        } else {
            uint16_t curr_virtio_desc = virtq->desc[virtio_desc].next;
            switch (virtio_cmd->type) {
                case VIRTIO_BLK_T_IN: {
                    // Copy the data from the data buffer to the virtio buffer
                    break;
                }
                case VIRTIO_BLK_T_OUT: {
                    // Free the data buffer
                    
                    curr_virtio_desc = virtq->desc[curr_virtio_desc].next;
                    *((uint8_t *)virtq->desc[curr_virtio_desc].addr) = VIRTIO_BLK_S_OK;
                    break;
                }
                case VIRTIO_BLK_T_FLUSH: {
                    curr_virtio_desc = virtq->desc[curr_virtio_desc].next;
                    *((uint8_t *)virtq->desc[curr_virtio_desc].addr) = VIRTIO_BLK_S_OK;
                    break;
                }
            }
        }
        
        virtio_blk_used_buffer(dev, virtio_desc);
    }

    virtio_blk_used_buffer_virq_inject(dev);
}

static virtio_device_funs_t functions = {
    .device_reset = virtio_blk_mmio_reset,
    .get_device_features = virtio_blk_mmio_get_device_features,
    .set_driver_features = virtio_blk_mmio_set_driver_features,
    .get_device_config = virtio_blk_mmio_get_device_config,
    .set_device_config = virtio_blk_mmio_set_device_config,
    .queue_notify = virtio_blk_mmio_queue_notify,
};

// @ericc: should these be hardcoded? can initialise via a configuration file
static void virtio_blk_config_init() 
{
    blk_config.capacity = VIRTIO_BLK_CAPACITY;
}

static void virtio_blk_cmd_store_init(unsigned int num_buffers)
{
    cmd_store.head = 0;
    cmd_store.tail = num_buffers - 1;
    cmd_store.num_free = num_buffers;
    for (unsigned int i = 0; i < num_buffers - 1; i++) {
        cmd_store.freelist[i] = i + 1;
    }
    cmd_store.freelist[num_buffers - 1] = -1;
}

static void data_region_init(unsigned int num_buffers, uintptr_t addr)
{
    data_region.avail_bitpos = 0;
    // Set all available bits to 1 to indicate it is available
    for (unsigned int i = 0; i < num_buffers; i++) {
        data_region.avail_bitmap[i] = MASK_ABOVE_POSITION_INCLUSIVE(0);
    }
    data_region.num_buffers = num_buffers;
    data_region.addr = addr;
}

void virtio_blk_init(struct virtio_device *dev,
                    struct virtio_queue_handler *vqs, size_t num_vqs,
                    size_t virq,
                    void **sddf_ring_handles, size_t sddf_ch,
                    uintptr_t data_region_addr) {
    dev->data.DeviceID = DEVICE_ID_VIRTIO_BLOCK;
    dev->data.VendorID = VIRTIO_MMIO_DEV_VENDOR_ID;
    dev->funs = &functions;
    dev->vqs = vqs;
    dev->num_vqs = num_vqs;
    dev->virq = virq;
    dev->sddf_ring_handles = sddf_ring_handles;
    dev->sddf_ch = sddf_ch;
    
    virtio_blk_config_init();
    virtio_blk_cmd_store_init(SDDF_BLK_NUM_DATA_BUFFERS);
    data_region_init(SDDF_BLK_NUM_DATA_BUFFERS, data_region_addr);
}