#ifndef GPU_HSA_THREAD_H
#define GPU_HSA_THREAD_H

#include <condition_variable>
#include <mutex>
#include <thread>

#include "fpga_header_functions.h"
#include "KotekanProcess.hpp"
#include "bufferContainer.hpp"
#include "gpuHSADeviceInterface.hpp"
#include "gpuHSACommandFactory.hpp"
#include "gpuHSACommand.hpp"
#include "signalContainer.hpp"

#include "hsa/hsa.h"
#include "hsa/hsa_ext_finalize.h"
#include "hsa/hsa_ext_amd.h"

class gpuHSAThread : public KotekanProcess {
public:
    gpuHSAThread(Config &config, bufferContainer &host_buffers, uint32_t gpu_id);
    virtual ~gpuHSAThread();

    void main_thread();

    void results_thread();

    virtual void apply_config(uint64_t fpga_seq);

private:

    bufferContainer &host_buffers;

    vector<signalContainer> final_signals;

    gpuHSACommandFactory * factory;
    gpuHSADeviceInterface * device;

    std::thread results_thread_handle;

    uint32_t _gpu_buffer_depth;

    uint32_t gpu_id;
};

#endif