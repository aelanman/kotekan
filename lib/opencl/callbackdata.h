#ifndef CALLBACKDATA_H
#define CALLBACKDATA_H

#include "gpu_command.h"
#include "buffer.h"
#include "pthread.h"

struct loopCounter {
    loopCounter() : iteration(0) {
        CHECK_ERROR( pthread_mutex_init(&lock, NULL) );
        CHECK_ERROR( pthread_cond_init(&cond, NULL) );
    }
    ~loopCounter() {
        CHECK_ERROR( pthread_mutex_destroy(&lock) );
        CHECK_ERROR( pthread_cond_destroy(&cond) );
    }
    int iteration;

    pthread_mutex_t lock;  // Lock for the is_ready function.
    pthread_cond_t cond;
};
struct buffer_id_lock {
   buffer_id_lock(){
        CHECK_ERROR( pthread_mutex_init(&lock, NULL) );
        CHECK_ERROR( pthread_cond_init(&mem_cond, NULL) );
        CHECK_ERROR( pthread_cond_init(&clean_cond, NULL) );
    }
    ~buffer_id_lock() {
        CHECK_ERROR( pthread_mutex_destroy(&lock) );
        CHECK_ERROR( pthread_cond_destroy(&mem_cond) );
        CHECK_ERROR( pthread_cond_destroy(&clean_cond) );
    }
    int clean = 0;
    int mem_in_use = 0;

    pthread_mutex_t lock;  // Lock for the is_ready function.
    pthread_cond_t mem_cond;
    pthread_cond_t clean_cond;
};
class callBackData {
public:
    callBackData();
    callBackData(cl_uint param_NumCommand);
    callBackData(callBackData &cb);
    ~callBackData();
    int buffer_id;
    int numCommands;
    int use_beamforming;
    double start_time;
    string unique_name;

    gpu_command ** listCommands;

    // Buffer objects
    struct Buffer * in_buf;
    struct Buffer * out_buf;
    struct Buffer * beamforming_out_buf;
    struct Buffer * rfi_out_buf;
    struct loopCounter * cnt;
    struct buffer_id_lock * buff_id_lock;
};

#endif // CALLBACKDATA_H
