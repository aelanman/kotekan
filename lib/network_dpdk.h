#ifndef NETWORK_DPDK
#define NETWORK_DPDK

#include "buffers.h"
#include "errors.h"
#include "fpga_header_functions.h"

// TODO Make these dynamic.
#define NUM_LINKS (4)
#define NUM_LCORES (4)
#define NUM_FREQ (1)

#ifdef __cplusplus
extern "C" {
#endif

struct networkDPDKArg {
    // Array of output buffers
    // TODO Not sure I like a triple pointer.
    // *[port][freq]
    struct Buffer *** buf;

    // These should take over the defines.
    int num_links;
    int num_lcores;
    int num_links_per_lcore;

    uint32_t num_links_in_group[NUM_LINKS];
    uint32_t link_id[NUM_LINKS];
    uint32_t port_offset[NUM_LCORES];

    int32_t timesamples_per_packet;
    int32_t samples_per_data_set;
    int32_t num_data_sets;
    int32_t num_gpu_frames;
    int32_t udp_packet_size;

    int dump_full_packets;
    int enable_shuffle;

    // Used for the vdif generation
    struct Buffer * vdif_buf;
};

struct LinkData {
    uint64_t seq;
    uint64_t last_seq;
    uint16_t stream_ID; // TODO just use the struct for this.
    stream_id_t s_stream_ID;
    int32_t first_packet;
    int32_t buffer_id;
    int32_t vdif_buffer_id;
    int32_t finished_buffer;
    int32_t data_id;
    int32_t dump_location;
};

struct NetworkDPDK {

    struct LinkData link_data[NUM_LINKS][NUM_FREQ];

    double start_time;
    double end_time;

    uint32_t data_id;
    uint32_t num_unused_cycles;

    struct networkDPDKArg * args;

    int vdif_time_set;
    uint64_t vdif_offset;  // Take (seq - offset) mod 5^8 to get data frame
    uint64_t vdif_base_time; // Add this to (seq - offset) / 5^8 to get time.
};

void* network_dpdk_thread(void * arg);

#ifdef __cplusplus
}
#endif

#endif