/*****************************************
@file
@brief HDF5 based visibility output files
- visFileH5
- visFile_H5Fast
*****************************************/
#ifndef VIS_FILE_H5_HPP
#define VIS_FILE_H5_HPP

#include "FrameView.hpp"      // for FrameView
#include "datasetManager.hpp" // for dset_id_t
#include "kotekanLogging.hpp" // for logLevel
#include "visBuffer.hpp"      // for VisFrameView
#include "visFile.hpp"        // for visFile
#include "visUtil.hpp"        // for time_ctype, freq_ctype, input_ctype, prod_ctype

#include <cstdint>                 // for uint32_t
#include <highfive/H5DataSet.hpp>  // for DataSet
#include <highfive/H5DataType.hpp> // for DataType
#include <highfive/H5File.hpp>     // for File
#include <map>                     // for map
#include <memory>                  // for unique_ptr
#include <stddef.h>                // for size_t
#include <string>                  // for string
#include <sys/types.h>             // for off_t
#include <vector>                  // for vector

/** @brief A CHIME correlator file.
 *
 * The class creates and manages writes to a CHIME style correlator output
 * file in the standard HDF5 format. It also manages the lock file.
 *
 * @author Richard Shaw
 **/
class visFileH5 : public visFile {

public:
    /**
     * Create an HDF5 file.
     *
     * This variant uses the datasetManager to look up properties of the
     * dataset that we are dealing with.
     *
     * @param name      Name of the file to write
     * @param log_level kotekan log level for any logging generated by the visFile instance
     * @param metadata  Textual metadata to write into the file.
     * @param dataset   ID of dataset we are writing.
     * @param max_time  Maximum number of times to write into the file.
     **/
    visFileH5(const std::string& name, const kotekan::logLevel log_level,
              const std::map<std::string, std::string>& metadata, dset_id_t dataset,
              size_t max_time);

    ~visFileH5();

    /**
     * @brief Extend the file to a new time sample.
     *
     * @param new_time The new time to add.
     * @return The index of the added time in the file.
     **/
    uint32_t extend_time(time_ctype new_time) override;

    /**
     * @brief Write a sample of data into the file at the given index.
     *
     * @param time_ind Time index to write into.
     * @param freq_ind Frequency index to write into.
     * @param frame Frame to write out.
     **/
    void write_sample(uint32_t time_ind, uint32_t freq_ind, const FrameView& frame) override;

    /**
     * @brief Return the current number of current time samples.
     *
     * @return The current number of time samples.
     **/
    size_t num_time() override;


protected:
    // This method contains initialisation of the file, that is deferred until
    // the first write into it is performed.
    // NOTE: the reason for having this (rather than doing it during
    // construction) is that it allows us to use virtual methods which allows
    // sharing of code between visFileH5 and subclasses
    virtual void deferred_init();

    // Create the time axis (separated for overloading)
    virtual void create_time_axis();

    // Helper to create datasets
    virtual void create_dataset(const std::string& name, const std::vector<std::string>& axes,
                                HighFive::DataType type);

    // Helper function to create an axis
    template<typename T>
    void create_axis(std::string name, const std::vector<T>& axis);

    // Create the index maps from the frequencies and the inputs
    void create_axes(const std::vector<freq_ctype>& freqs, const std::vector<input_ctype>& inputs,
                     const std::vector<prod_ctype>& prods, size_t num_ev);

    // Create the main visibility holding datasets
    void create_datasets();

    // Get datasets
    HighFive::DataSet dset(const std::string& name);
    size_t length(const std::string& axis_name);

    // Number of eigenvalues
    size_t num_ev;

    // Maximum number of time samples to save
    size_t _max_time;

    // Pointer to the underlying HighFive file
    std::unique_ptr<HighFive::File> file;

    std::string lock_filename;
};


/**
 * @brief A correlator output file with fast direct writing..
 *
 * This class writes HDF5 formatted files, but for improved speed bypasses HDF5
 * when writing out data. To do this it uses contiguous datasets, which means
 * that the files are pre-allocated to their maximum size. On close, the number
 * of time samples written is written into an attribute on the file called
 * `num_time`.
 *
 * Note that we rely on the behaviour of the filesystem to return 0 in
 * allocated but unwritten parts of the files to give zero weights for
 * unwritten data.
 *
 * @author Richard Shaw
 **/
class visFileH5Fast : public visFileH5 {

public:
    /**
     * Create an HDF5 file that uses faster IO.
     *
     * This variant uses the datasetManager to look up properties of the
     * dataset that we are dealing with.
     *
     * @param name      Name of the file to write
     * @param log_level kotekan log level for any logging generated by the visFile instance
     * @param metadata  Textual metadata to write into the file.
     * @param dataset   ID of dataset we are writing.
     * @param max_time  Maximum number of times to write into the file.
     **/
    visFileH5Fast(const std::string& name, const kotekan::logLevel log_level,
                  const std::map<std::string, std::string>& metadata, dset_id_t dataset,
                  size_t max_time);

    // Write out the number of times as we are destroyed.
    ~visFileH5Fast();

    /**
     * @brief Extend the file to a new time sample.
     *
     * @param new_time The new time to add.
     * @return The index of the added time in the file.
     **/
    uint32_t extend_time(time_ctype new_time) override;

    /**
     * @brief Write a sample of data into the file at the given index.
     *
     * @param time_ind Time index to write into.
     * @param freq_ind Frequency index to write into.
     * @param frame Frame to write out.
     **/
    void write_sample(uint32_t time_ind, uint32_t freq_ind, const FrameView& frame) override;

    size_t num_time() override;

    /**
     * @brief Remove the time sample from the active set being written to.
     *
     * This explicit flushes the requested time sample and evicts it from the
     * page cache.
     *
     * @param time_ind Sample to cleanup.
     **/
    void deactivate_time(uint32_t time_ind) override;

protected:
    // Override initialisation for the raw file
    void deferred_init() override;

    // Create the time axis (separated for overloading)
    void create_time_axis() override;

    // Helper to create datasets
    void create_dataset(const std::string& name, const std::vector<std::string>& axes,
                        HighFive::DataType type) override;

    // Calculate offsets into the file for each dataset, and open it
    void setup_raw();

    /**
     * @brief  Helper routine for writing data into the file
     *
     * @param dset_base Offset of dataset in file
     * @param ind       The index into the file dataset in chunks.
     * @param n         The size of the chunk in elements.
     * @param vec       The data to write out.
     **/
    template<typename T>
    bool write_raw(off_t dset_base, int ind, size_t n, const std::vector<T>& vec);

    /**
     * @brief  Helper routine for writing data into the file
     *
     * @param dset_base Offset of dataset in file
     * @param ind       The index into the file dataset in chunks.
     * @param n         The size of the chunk in elements.
     * @param data       The data to write out.
     **/
    template<typename T>
    bool write_raw(off_t dset_base, int ind, size_t n, const T* data);

    /**
     * @brief Start an async flush to disk
     *
     * @param dset_base Offset of dataset in file
     * @param ind       The index into the file dataset in time.
     * @param n         The size of the region to flush in bytes.
     **/
    void flush_raw_async(off_t dset_base, int ind, size_t n);

    /**
     * @brief Start a synchronised flush to disk and evict any clean pages.
     *
     * @param dset_base Offset of dataset in file
     * @param ind       The index into the file dataset in time.
     * @param n         The size of the region to flush in bytes.
     **/
    void flush_raw_sync(off_t dset_base, int ind, size_t n);

    // Save the size for when we are outside of HDF5 space
    size_t nfreq, nprod, ninput, nev, ntime = 0;

    // File descriptor of file.
    int fd;

    // Store offsets into the file for writing
    off_t vis_offset, weight_offset, gcoeff_offset, gexp_offset, eval_offset, evec_offset,
        erms_offset, time_offset;
};


// These templated functions are needed in order to tell HighFive how the
// various structs are converted into HDF5 datatypes
const size_t DSET_ID_LEN = 33; // Length of the string used to represent dataset IDs
struct dset_id_str {
    char hash[DSET_ID_LEN];
};
namespace HighFive {
template<>
DataType create_datatype<freq_ctype>();
template<>
DataType create_datatype<time_ctype>();
template<>
DataType create_datatype<input_ctype>();
template<>
DataType create_datatype<prod_ctype>();
template<>
DataType create_datatype<cfloat>();

// \cond NO_DOC
// Fixed length string to store dataset ID
template<>
inline AtomicType<dset_id_str>::AtomicType() {
    _hid = H5Tcopy(H5T_C_S1);
    H5Tset_size(_hid, DSET_ID_LEN);
}
// \endcond
}; // namespace HighFive


#endif
