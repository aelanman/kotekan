#include "valve.hpp"

#include <string>
#include <pthread.h>
#include <cstring>
#include <signal.h>

#include "fmt.hpp"

#include "visUtil.hpp"
#include "buffer.h"
#include "KotekanProcess.hpp"
#include "bufferContainer.hpp"
#include "prometheusMetrics.hpp"


REGISTER_KOTEKAN_PROCESS(Valve);

Valve::Valve(Config& config,
             const std::string& unique_name,
             bufferContainer &buffer_container) :
    KotekanProcess(config, unique_name, buffer_container,
                   std::bind(&Valve::main_thread, this)) {

    _dropped_total = 0;

    _buf_in = get_buffer("in_buf");
    register_consumer(_buf_in, unique_name.c_str());
    _buf_out = get_buffer("out_buf");
    register_producer(_buf_out, unique_name.c_str());
}

void Valve::main_thread() {
    frameID frame_id_in(_buf_in);
    frameID frame_id_out(_buf_out);

    while (!stop_thread) {
        // Fetch a new frame and get its sequence id
        uint8_t* frame_in = wait_for_full_frame(_buf_in, unique_name.c_str(),
                                            frame_id_in);
        if(frame_in == nullptr) break;

        // check if there is space for it in the output buffer
        if (is_frame_empty(_buf_out, frame_id_out)) {
            try {
                copy_frame(_buf_in, frame_id_in, _buf_out, frame_id_out);
            } catch (std::exception& e) {
                ERROR("Failure copying frame: %s\nExiting...", e.what());
                raise(SIGINT);
            }
            mark_frame_full(_buf_out, unique_name.c_str(), frame_id_out++);
        } else {
            WARN("Output buffer full. Dropping incoming frame %d.",
                 frame_id_in);
            prometheusMetrics::instance().add_process_metric(
                        "dropped_frames_total", unique_name, ++_dropped_total);
        }
        mark_frame_empty(_buf_in, unique_name.c_str(), frame_id_in++);
    }
}

// mostly copied from visFrameView
void Valve::copy_frame(Buffer* buf_src, int frame_id_src,
                       Buffer* buf_dest, int frame_id_dest) {
    allocate_new_metadata_object(buf_dest, frame_id_dest);

    // Buffer sizes must match exactly
    if (buf_src->frame_size != buf_dest->frame_size) {
        std::string msg = fmt::format(
            "Buffer sizes must match for direct copy (src %i != dest %i).",
            buf_src->frame_size, buf_dest->frame_size);
        throw std::runtime_error(msg);
    }

    // Metadata sizes must match exactly
    if (buf_src->metadata[frame_id_src]->metadata_size !=
        buf_dest->metadata[frame_id_dest]->metadata_size) {
        std::string msg = fmt::format(
            "Metadata sizes must match for direct copy (src %i != dest %i).",
            buf_src->metadata[frame_id_src]->metadata_size,
            buf_dest->metadata[frame_id_dest]->metadata_size);
        throw std::runtime_error(msg);
    }

    // Calculate the number of consumers on the source buffer and copy over the
    // data. Keep a lock on the buffer to prevent consumers from joining.
    int err = pthread_mutex_lock(&buf_src->lock);
    if (err) {
        std::string msg = fmt::format("Failure locking input buffer: {}",
                                      std::strerror(err));
        throw std::runtime_error(msg);
    }

    int num_consumers = 0;
    for (int i = 0; i < MAX_CONSUMERS; ++i) {
        if (buf_src->consumers[i].in_use == 1) {
            num_consumers++;
        }
    }

    // Copy or transfer the data part.
    if (num_consumers == 1) {
        err = pthread_mutex_unlock(&buf_src->lock);
        if (err) {
            std::string msg = fmt::format("Failure unlocking input buffer: {}",
                                          std::strerror(err));
            throw std::runtime_error(msg);
        }
        // Transfer frame contents with directly...
        swap_frames(buf_src, frame_id_src, buf_dest, frame_id_dest);
    } else if (num_consumers > 1) {
        // Copy the frame data over, leaving the source intact
        std::memcpy(buf_dest->frames[frame_id_dest],
                    buf_src->frames[frame_id_src], buf_src->frame_size);
        err = pthread_mutex_unlock(&buf_src->lock);
        if (err) {
            std::string msg = fmt::format("Failure unlocking input buffer: {}",
                                          std::strerror(err));
            throw std::runtime_error(msg);
        }
    }

    // Copy over the metadata
    std::memcpy(buf_dest->metadata[frame_id_dest]->metadata,
                buf_src->metadata[frame_id_src]->metadata,
                buf_src->metadata[frame_id_src]->metadata_size);

}