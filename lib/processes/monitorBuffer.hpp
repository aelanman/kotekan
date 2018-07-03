/**
 * @file
 * @brief Process to watch a buffer or buffers and exit the system if it doesn't
 *        get new data within a set timeout
 *  - monitorBuffer : public KotekanProcess
 */

#ifndef MONITOR_BUFFER_H
#define MONITOR_BUFFER_H

#include <vector>

#include "KotekanProcess.hpp"
#include "bufferContainer.hpp"

/**
 * @class monitorBuffer
 * @brief Watches a set of buffers to make sure they are getting data
 *        within a given timeout, otherwise exits.
 *
 * @note This will not detect buffers which never get data, the buffer
 *       must get at least one frame before this system will start checking.
 *
 * @par Buffers
 * @buffer bufs An array of kotekan buffers
 *  @buffer_format Any
 *  @buffer_metadata Any
 *
 * @conf timeout       Int, default 60. Time to wait in seconds for new frame
 *                     before exiting.   Must be > 1
 */
class monitorBuffer : public KotekanProcess {
public:
    /// Common constructor
    monitorBuffer(Config& config, const string& unique_name,
                         bufferContainer &buffer_container);

    /// Destructor
    virtual ~monitorBuffer();

    /// Watches the list of buffers for timeouts
    void main_thread();

    /// Deprecated
    virtual void apply_config(uint64_t fpga_seq);

private:
    /// Internal list of buffers to check
    std::vector<struct Buffer*> buffers;

    /// The timeout after which kotekan exits.
    int timeout;
};

#endif