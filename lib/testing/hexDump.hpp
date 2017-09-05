#ifndef HEX_DUMP_H
#define HEX_DUMP_H

#include "buffers.h"
#include "KotekanProcess.hpp"
#include "errors.h"
#include "util.h"
#include <unistd.h>

/*
 * Checks that the contents of "buf" match the complex number given by "real" and "imag"
 * Configuration options
 * "buf": String with the name of the buffer to check
 * "real": Expected real value (int)
 * "imag": Expected imaginary value (int)
 */

class hexDump : public KotekanProcess {
public:
    hexDump(Config &config,
                  const string& unique_name,
                  bufferContainer &buffer_container);
    ~hexDump();
    void apply_config(uint64_t fpga_seq) override;
    void main_thread();
private:
    struct Buffer *buf;
    int32_t len;
    int32_t offset;
};

#endif