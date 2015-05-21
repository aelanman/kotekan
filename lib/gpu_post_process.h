#ifndef GPU_POST_PROCESS
#define GPU_POST_PROCESS

struct gpuPostProcessThreadArg {
    struct Config * config;
    struct Buffer * in_buf;
    struct Buffer * out_buf;
};

// A TCP frame contains this header followed by the visibilities, and flags.
// -- HEADER:sizeof(TCP_frame_header) --
// -- VISIBILITIES:n_corr * n_freq * sizeof(complex_int_t) --
// -- PER_FREQUENCY_DATA:n_freq * sizeof(per_frequency_data) --
// -- PER_ELEMENT_DATA:n_freq * n_elem * sizeof(per_element_data) --
// -- VIS_WEIGHT:n_corr * n_freq * sizeof(uint8_t) --
#pragma pack(1)
struct stream_id {
    unsigned int link_id : 8;
    unsigned int slot_id : 8;
    unsigned int crate_id : 8;
    unsigned int reserved : 8;
};

struct tcp_frame_header {
    uint32_t fpga_seq_number;
    uint32_t num_freq;
    uint32_t num_vis; // The number of visibilities per frequency.
    uint32_t num_elements;
    uint32_t num_links; // The number of GPU links in this frame.

    struct timeval cpu_timestamp; // The time stamp as set by the GPU correlator - not accurate!
};

struct per_frequency_data {
  struct stream_id stream_id;
  uint32_t index; // The position in the FPGA frame which is assoicated with
                  // this frequency.
  uint32_t lost_packet_count;
  uint32_t rfi_count;
};

struct per_element_data {
  uint32_t fpga_adc_count;
  uint32_t fpga_fft_count;
  uint32_t fpga_scalar_count;
};
#pragma pack(0)

void gpu_post_process_thread(void * arg);

#endif