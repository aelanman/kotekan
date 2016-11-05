#include "fpga_header_functions.h"

stream_id_t extract_stream_id(const uint16_t encoded_stream_id)
{
    stream_id_t stream_id;

    stream_id.link_id = encoded_stream_id & 0x000F;
    stream_id.slot_id = (encoded_stream_id & 0x00F0) >> 4;
    stream_id.create_id = (encoded_stream_id & 0x0F00) >> 8;
    stream_id.unused = (encoded_stream_id & 0xF000) >> 12;

    return stream_id;
}

uint16_t encode_stream_id(const stream_id_t s_stream_id)
{
    uint16_t stream_id;

    stream_id = (s_stream_id.link_id & 0xF) +
                ((s_stream_id.slot_id & 0xF)  << 4) +
                ((s_stream_id.create_id & 0xF) << 8) +
                ((s_stream_id.unused & 0xF) << 12);

    return stream_id;
}

float freq_from_bin(const int bin)
{
    return 800.0 - (float)bin * 400.0/1024.0;
}