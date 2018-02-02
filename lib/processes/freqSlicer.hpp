/*****************************************
@file
@brief Processes for splitting and subsetting visibility data by frequency.
- freqSplit : public KotekanProcess

@todo Add process for frequency subsetting
*****************************************/
#ifndef FREQ_SLICER_HPP
#define FREQ_SLICER_HPP

#include <unistd.h>
#include "fpga_header_functions.h"
#include "buffer.h"
#include "KotekanProcess.hpp"
#include "visFile.hpp"
#include "errors.h"
#include "util.h"
#include "visUtil.hpp"


// Split frequencies into upper and lower halves
/**
 * @class freqSplit
 * @brief Separate a visBuffer stream into two by selecting frequencies in the upper and lower half of the band.
 *
 * This task takes data coming out of a visBuffer stream and separates it into
 * two streams. It selects which frames to copy to which buffer by assigning
 * frequencies in the upper and lower half of the CHIME band to different buffer
 * streams.
 *
 * @par Buffers
 * @buffer output_buffers The two buffers containing the respective upper or lower band frequencies
 *         @buffer_format visBuffer structured
 *         @buffer_metadata visMetadata
 * @buffer input_buffer The buffer to be split
 *         @buffer_format visBuffer structured
 *         @buffer_metadata visMetadata
 *
 * @conf  num_elements      Int. The number of elements (i.e. inputs) in the
 *                          correlator data (read from "/")
 * @conf  num_eigenvectors  Int. The number of eigenvectors to be stored
 *
 * @author Mateus Fandino
 */
class freqSplit : public KotekanProcess {

public:

    // Default constructor
    freqSplit(Config &config,
              const string& unique_name,
              bufferContainer &buffer_container);

    void apply_config(uint64_t fpga_seq);

    // Main loop for the process
    void main_thread();

private:

    // Parameters saved from the config files
    size_t num_elements, num_eigenvectors;

    // Vector of the buffers we are using and their current frame ids.
    std::vector<std::pair<Buffer*, unsigned int>> output_buffers;
    Buffer * input_buffer;

};

#endif
