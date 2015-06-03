// CASPER tools, according to JF Cliche, store complex pairs in a Real,Imaginary order, so when we have a 1 B packed pair,
// the order should be:
//   RRRRIIII
// where Real values are in the high nibble.  Previous versions of code had used IIIIRRRR ordering within a single uchar/Byte

#include <complex.h>    // I is used to get complex parts (not i (nor j for engineers)--I suppose they realized those get used as simple loop counters often?)

#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/time.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <assert.h>
#include "SFMT-src-1.4/SFMT.h" //Mersenne Twister library

#define NUM_CL_FILES                            4u //3u when not using the time_shift kernel
#define OPENCL_FILENAME_0                       "time_shift.cl"
#define OPENCL_FILENAME_1                       "pairwise_correlator_c_groups.cl" /* _optimizing1.cl .cl" */
#define OPENCL_FILENAME_2                       "offsetAccumulator_gating_multiple_outputs_freq_dependent.cl"
#define OPENCL_FILENAME_2b                      "offsetAccumulatorXOR_gating.cl"
#define OPENCL_FILENAME_3                       "preseed_multifreq_dep_gating_multiple_outputs.cl"
// #define OPENCL_FILENAME_0                       "/home/pklages/dev/ch_gpu/pek_versions/gating/time_shift.cl"
// #define OPENCL_FILENAME_1                       "/home/pklages/dev/ch_gpu/pek_versions/gating/pairwise_correlator_c_groups.cl" /* _optimizing1.cl .cl" */
// #define OPENCL_FILENAME_2                       "/home/pklages/dev/ch_gpu/pek_versions/gating/offsetAccumulator_gating_multiple_outputs_freq_dependent.cl"
// #define OPENCL_FILENAME_2b                      "/home/pklages/dev/ch_gpu/pek_versions/gating/offsetAccumulatorXOR_gating.cl"
// #define OPENCL_FILENAME_3                       "/home/pklages/dev/ch_gpu/pek_versions/gating/preseed_multifreq_dep_gating_multiple_outputs.cl"

#define HI_NIBBLE(b)                            (((b) >> 4) & 0x0F)
#define LO_NIBBLE(b)                            ((b) & 0x0F)

#define SDK_SUCCESS                             0u

// //////////////////////change these as desired/////////////////////////////////////////
#define NUM_ELEM                                256//32//256 //32u //minimum needs to be 32 for the kernels
#define NUM_FREQ                                8//64   //64u //63u
#define ACTUAL_NUM_ELEM                         256//16//256 //16u
#define ACTUAL_NUM_FREQ                         8//128   //128u //126u //product of ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ should equal NUM_ELEM*NUM_FREQ
#define HDF5_FREQ                               8//128   //1024 is too large when having 128 datasets and 1024 frequency bands for the 256 element correlator
#define UPPER_TRIANGLE                          1
#define INTERLEAVED                             0
#define XOR_MANUAL                              0
#define TIMER_FOR_PROCESSING_ONLY               0

#define NUM_TIME_BINS                           128
#define NUM_GATING_GROUPS                       2 //will always be less than the number of Data sets... (Was half NUM_DATA_SETS; NUM_DATA_SETS was NUM_GATING_GROUPS*2)
#define NUM_ZONES                               3 //for each gating group there are multiple freq bands that can use up to NUM_ZONES outputs each (2 would mean on/off--2 zones (like the old version)) These are assigned by the mask
//#define NUM_DATA_SETS                           ACTUAL_NUM_FREQ*NUM_GATING_GROUPS*NUM_ZONES
#define NUM_DATA_SETS                           NUM_GATING_GROUPS*NUM_ZONES

#define NUM_TIMESAMPLES                         2*128*256//128u*8u*256u //
//#define NUM_REPEATS_GPU                         1000u

#define GATE_PERIOD_IN_10ns_UNITS               1000 // i.e. 1000 x 10ns = 10000 ns = 0.01 ms
// ////////////////////////////////////////////////////////////////////////////////////

#define N_STAGES                                2 //write to CL_Mem, Kernel (Read is done after many runs since answers are accumulated)
#define N_QUEUES                                2 //have 2 separate queues so transfer and process paths can be queued nicely

#define DEBUG                                   0
#define DEBUG_GENERATOR                         0

#define TIME_FOR_TIMESTEP_IN_10ns_UNITS         256u
//check pagesize:
//getconf PAGESIZE
// result: 4096
#define PAGESIZE_MEM                            4096u
#define TIME_ACCUM                              256u
#define BASE_TIMESAMPLES_ACCUM                  256u

//enumerations/definitions: don't change
#define GENERATE_DATASET_CONSTANT               1u
#define GENERATE_DATASET_RAMP_UP                2u
#define GENERATE_DATASET_RAMP_DOWN              3u
#define GENERATE_DATASET_RANDOM_SEEDED          4u
#define GENERATE_DATASET_RANDOM_NORMAL          5u
#define GENERATE_DATASET_RAMP_UP_WITH_TIME      6u
#define GENERATE_DATASET_RAMP_DOWN_WITH_TIME    7u
#define ALL_FREQUENCIES                        -1

//parameters for data generator: you can change these. (Values will be shifted and clipped as needed, so these are signed 4bit numbers for input)
#define GEN_TYPE                                GENERATE_DATASET_RANDOM_SEEDED//GENERATE_DATASET_RAMP_DOWN_WITH_TIME
#define GEN_DEFAULT_SEED                        42u
#define GEN_DEFAULT_RE                          0u
#define GEN_DEFAULT_IM                          0u
#define GEN_INITIAL_RE                          0u //-8
#define GEN_INITIAL_IM                          0u //7
#define GEN_FREQ                                ALL_FREQUENCIES
#define GEN_REPEAT_RANDOM                       1u

#define CHECKING_VERBOSE                        1u
#define VERIFY_RESULTS                          1u
#define RMS_VAL                                 3//4.595 //2.2 bits

double e_time(void){
    static struct timeval now;
    gettimeofday(&now, NULL);
    return (double)(now.tv_sec  + now.tv_usec/1000000.0);
}

//error codes from an amd firepro demo:
char* oclGetOpenCLErrorCodeStr(cl_int input)
{
    int errorCode = (int)input;
    switch(errorCode)
    {
        case CL_SUCCESS:
            return (char*) "CL_SUCCESS";
        case CL_DEVICE_NOT_FOUND:
            return (char*) "CL_DEVICE_NOT_FOUND";
        case CL_DEVICE_NOT_AVAILABLE:
            return (char*) "CL_DEVICE_NOT_AVAILABLE";
        case CL_COMPILER_NOT_AVAILABLE:
            return (char*) "CL_COMPILER_NOT_AVAILABLE";
        case CL_MEM_OBJECT_ALLOCATION_FAILURE:
            return (char*) "CL_MEM_OBJECT_ALLOCATION_FAILURE";
        case CL_OUT_OF_RESOURCES:
            return (char*) "CL_OUT_OF_RESOURCES";
        case CL_OUT_OF_HOST_MEMORY:
            return (char*) "CL_OUT_OF_HOST_MEMORY";
        case CL_PROFILING_INFO_NOT_AVAILABLE:
            return (char*) "CL_PROFILING_INFO_NOT_AVAILABLE";
        case CL_MEM_COPY_OVERLAP:
            return (char*) "CL_MEM_COPY_OVERLAP";
        case CL_IMAGE_FORMAT_MISMATCH:
            return (char*) "CL_IMAGE_FORMAT_MISMATCH";
        case CL_IMAGE_FORMAT_NOT_SUPPORTED:
            return (char*) "CL_IMAGE_FORMAT_NOT_SUPPORTED";
        case CL_BUILD_PROGRAM_FAILURE:
            return (char*) "CL_BUILD_PROGRAM_FAILURE";
        case CL_MAP_FAILURE:
            return (char*) "CL_MAP_FAILURE";
        case CL_MISALIGNED_SUB_BUFFER_OFFSET:
            return (char*) "CL_MISALIGNED_SUB_BUFFER_OFFSET";
        case CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST:
            return (char*) "CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST";
        case CL_COMPILE_PROGRAM_FAILURE:
            return (char*) "CL_COMPILE_PROGRAM_FAILURE";
        case CL_LINKER_NOT_AVAILABLE:
            return (char*) "CL_LINKER_NOT_AVAILABLE";
        case CL_LINK_PROGRAM_FAILURE:
            return (char*) "CL_LINK_PROGRAM_FAILURE";
        case CL_DEVICE_PARTITION_FAILED:
            return (char*) "CL_DEVICE_PARTITION_FAILED";
        case CL_KERNEL_ARG_INFO_NOT_AVAILABLE:
            return (char*) "CL_KERNEL_ARG_INFO_NOT_AVAILABLE";
        case CL_INVALID_VALUE:
            return (char*) "CL_INVALID_VALUE";
        case CL_INVALID_DEVICE_TYPE:
            return (char*) "CL_INVALID_DEVICE_TYPE";
        case CL_INVALID_PLATFORM:
            return (char*) "CL_INVALID_PLATFORM";
        case CL_INVALID_DEVICE:
            return (char*) "CL_INVALID_DEVICE";
        case CL_INVALID_CONTEXT:
            return (char*) "CL_INVALID_CONTEXT";
        case CL_INVALID_QUEUE_PROPERTIES:
            return (char*) "CL_INVALID_QUEUE_PROPERTIES";
        case CL_INVALID_COMMAND_QUEUE:
            return (char*) "CL_INVALID_COMMAND_QUEUE";
        case CL_INVALID_HOST_PTR:
            return (char*) "CL_INVALID_HOST_PTR";
        case CL_INVALID_MEM_OBJECT:
            return (char*) "CL_INVALID_MEM_OBJECT";
        case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR:
            return (char*) "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
        case CL_INVALID_IMAGE_SIZE:
            return (char*) "CL_INVALID_IMAGE_SIZE";
        case CL_INVALID_SAMPLER:
            return (char*) "CL_INVALID_SAMPLER";
        case CL_INVALID_BINARY:
            return (char*) "CL_INVALID_BINARY";
        case CL_INVALID_BUILD_OPTIONS:
            return (char*) "CL_INVALID_BUILD_OPTIONS";
        case CL_INVALID_PROGRAM:
            return (char*) "CL_INVALID_PROGRAM";
        case CL_INVALID_PROGRAM_EXECUTABLE:
            return (char*) "CL_INVALID_PROGRAM_EXECUTABLE";
        case CL_INVALID_KERNEL_NAME:
            return (char*) "CL_INVALID_KERNEL_NAME";
        case CL_INVALID_KERNEL_DEFINITION:
            return (char*) "CL_INVALID_KERNEL_DEFINITION";
        case CL_INVALID_KERNEL:
            return (char*) "CL_INVALID_KERNEL";
        case CL_INVALID_ARG_INDEX:
            return (char*) "CL_INVALID_ARG_INDEX";
        case CL_INVALID_ARG_VALUE:
            return (char*) "CL_INVALID_ARG_VALUE";
        case CL_INVALID_ARG_SIZE:
            return (char*) "CL_INVALID_ARG_SIZE";
        case CL_INVALID_KERNEL_ARGS:
            return (char*) "CL_INVALID_KERNEL_ARGS";
        case CL_INVALID_WORK_DIMENSION:
            return (char*) "CL_INVALID_WORK_DIMENSION";
        case CL_INVALID_WORK_GROUP_SIZE:
            return (char*) "CL_INVALID_WORK_GROUP_SIZE";
        case CL_INVALID_WORK_ITEM_SIZE:
            return (char*) "CL_INVALID_WORK_ITEM_SIZE";
        case CL_INVALID_GLOBAL_OFFSET:
            return (char*) "CL_INVALID_GLOBAL_OFFSET";
        case CL_INVALID_EVENT_WAIT_LIST:
            return (char*) "CL_INVALID_EVENT_WAIT_LIST";
        case CL_INVALID_EVENT:
            return (char*) "CL_INVALID_EVENT";
        case CL_INVALID_OPERATION:
            return (char*) "CL_INVALID_OPERATION";
        case CL_INVALID_GL_OBJECT:
            return (char*) "CL_INVALID_GL_OBJECT";
        case CL_INVALID_BUFFER_SIZE:
            return (char*) "CL_INVALID_BUFFER_SIZE";
        case CL_INVALID_MIP_LEVEL:
            return (char*) "CL_INVALID_MIP_LEVEL";
        case CL_INVALID_GLOBAL_WORK_SIZE:
            return (char*) "CL_INVALID_GLOBAL_WORK_SIZE";
        case CL_INVALID_PROPERTY:
            return (char*) "CL_INVALID_PROPERTY";
        case CL_INVALID_IMAGE_DESCRIPTOR:
            return (char*) "CL_INVALID_IMAGE_DESCRIPTOR";
        case CL_INVALID_COMPILER_OPTIONS:
            return (char*) "CL_INVALID_COMPILER_OPTIONS";
        case CL_INVALID_LINKER_OPTIONS:
            return (char*) "CL_INVALID_LINKER_OPTIONS";
        case CL_INVALID_DEVICE_PARTITION_COUNT:
            return (char*) "CL_INVALID_DEVICE_PARTITION_COUNT";
        default:
            return (char*) "unknown error code";
    }

    return (char*) "unknown error code";
}

int offset_and_clip_value(int input_value, int offset_value, int min_val, int max_val){
    int offset_and_clipped = input_value + offset_value;
    if (offset_and_clipped > max_val)
        offset_and_clipped = max_val;
    else if (offset_and_clipped < min_val)
        offset_and_clipped = min_val;
    return(offset_and_clipped);
}

int offset_and_flag_value(int input_value, int offset_value, int min_val, int max_val, int flag_val){
    int offset_and_flagged = input_value + offset_value;
    if (offset_and_flagged > max_val)
        offset_and_flagged = flag_val;
    else if (offset_and_flagged < min_val)
        offset_and_flagged = flag_val;
    return(offset_and_flagged);
}


//Box-Muller Gaussian Distribution.
//based on method found in Numerical Recipes in C, but returns a rounded int instead, and uses Mersenne Twister values
//polar form of Box-Muller form
//Using uniformly distributed random numbers returns a value with mean, mu, and standard dev, sigma
//keeps the second val for next time to be less wasteful (but therefore is not thread safe)
int normal_dist_Box_Muller(sfmt_t *sfmt, double mu, double sigma){
    static double second_value = 0.0;
    double rand_val;
    int out_val;
    if (second_value == 0.0){
        double u, v, length;
        do {
            u = sfmt_genrand_res53(sfmt)*2.0 -1.0;
            v = sfmt_genrand_res53(sfmt)*2.0 -1.0;
            length = u*u+v*v;
        } while (length == 0 || length >= 1.0);
        //printf("length: %f ",length);
        double coeff = sqrt(-2.0*log(length)/length);
        rand_val = coeff*v*sigma + mu;
        second_value = coeff*u;
        out_val = (int)(round(rand_val));
        //printf("1) %f\n",rand_val);
        return out_val;
    }
    else{
        rand_val = second_value*sigma + mu;
        second_value = 0.0;
        out_val = (int)(round(rand_val));
        //printf("2) %f\n",rand_val);
        return out_val;
    }
}

void generate_char_data_set(int generation_Type,
                            int random_seed,
                            int default_real,
                            int default_imaginary,
                            int initial_real,
                            int initial_imaginary,
                            int single_frequency,
                            int num_timesteps,
                            int num_frequencies,
                            int num_elements,
                            int num_data_sets,
                            unsigned char *packed_data_set){

    sfmt_t sfmt; //for the Mersenne Twister
    if (single_frequency > num_frequencies || single_frequency < 0)
        single_frequency = ALL_FREQUENCIES;

    //printf("single_frequency: %d \n",single_frequency);
    default_real =offset_and_clip_value(default_real,8,0,15);
    default_imaginary = offset_and_clip_value(default_imaginary,8,0,15);
    initial_real = offset_and_clip_value(initial_real,8,0,15);
    initial_imaginary = offset_and_clip_value(initial_imaginary,8,0,15);
    unsigned char clipped_offset_default_real = (unsigned char) default_real;
    unsigned char clipped_offset_default_imaginary = (unsigned char) default_imaginary;
    unsigned char clipped_offset_initial_real = (unsigned char) initial_real;
    unsigned char clipped_offset_initial_imaginary = (unsigned char) initial_imaginary;
    unsigned char temp_output;


    //printf("clipped_offset_initial_real: %d, clipped_offset_initial_imaginary: %d, clipped_offset_default_real: %d, clipped_offset_default_imaginary: %d\n", clipped_offset_initial_real, clipped_offset_initial_imaginary, clipped_offset_default_real, clipped_offset_default_imaginary);
    for (int m = 0; m < num_data_sets; m++){
        if (generation_Type == GENERATE_DATASET_RANDOM_SEEDED){
            sfmt_init_gen_rand(&sfmt, random_seed);
            //srand(random_seed);
        }

        for (int k = 0; k < num_timesteps; k++){
            //printf("k: %d\n",k);
            if (generation_Type == GENERATE_DATASET_RANDOM_SEEDED && GEN_REPEAT_RANDOM){
                sfmt_init_gen_rand(&sfmt, random_seed);
                //srand(random_seed);
            }
            for (int j = 0; j < num_frequencies; j++){
                if (DEBUG_GENERATOR && k == 0)
                    printf("j: %d Vals: ",j);
                for (int i = 0; i < num_elements; i++){
                    int currentAddress = m*num_timesteps*num_frequencies*num_elements +k*num_frequencies*num_elements + j*num_elements + i;
                    unsigned char new_real;
                    unsigned char new_imaginary;
                    switch (generation_Type){
                        case GENERATE_DATASET_CONSTANT:
                            new_real = clipped_offset_initial_real;
                            new_imaginary = clipped_offset_initial_imaginary;
                            break;
                        case GENERATE_DATASET_RAMP_UP:
                            new_real = (j+clipped_offset_initial_real+i)%16;
                            new_imaginary = (j+clipped_offset_initial_imaginary+i)%16;
                            break;
                        case GENERATE_DATASET_RAMP_DOWN:
                            new_real = 15-((j+clipped_offset_initial_real+i)%16);
                            new_imaginary = 15 - ((j+clipped_offset_initial_imaginary+i)%16);
                            break;
                        case GENERATE_DATASET_RANDOM_SEEDED:
                            new_real = (unsigned char)(floor(sfmt_genrand_res53(&sfmt)*16));//rand()%16; //to put the pseudorandom value in the range 0-15
                            new_imaginary = (unsigned char)(floor(sfmt_genrand_res53(&sfmt)*16));//rand()%16;
                            break;
                        case GENERATE_DATASET_RANDOM_NORMAL:
                            new_real = (unsigned char)(offset_and_clip_value(normal_dist_Box_Muller(&sfmt,0,RMS_VAL),8,0,15));
                            new_imaginary = (unsigned char)(offset_and_clip_value(normal_dist_Box_Muller(&sfmt,0,RMS_VAL),8,0,15));
                            //printf("%d %d\n",new_real, new_imaginary);
                            break;
                        case GENERATE_DATASET_RAMP_UP_WITH_TIME:
                            new_real = (clipped_offset_initial_real+k)%16;
                            new_imaginary = (clipped_offset_initial_imaginary+k)%16;
                            break;
                        case GENERATE_DATASET_RAMP_DOWN_WITH_TIME:
                            new_real = (clipped_offset_initial_real-k)%16;
                            new_imaginary = (clipped_offset_initial_imaginary-k)%16;
                            break;
                        default: //shouldn't happen, but in case it does, just assign the default values everywhere
                            new_real = clipped_offset_default_real;
                            new_imaginary = clipped_offset_default_imaginary;
                            break;
                    }

                    if (single_frequency == ALL_FREQUENCIES){
                        temp_output = ((new_real<<4) & 0xF0) + (new_imaginary & 0x0F);
                        if (XOR_MANUAL){
                            temp_output = temp_output ^ 0x88; //bit flip on sign bit shifts the value by 8: makes unsigned into signed and vice versa.  Currently turns back into signed
                        }
                        packed_data_set[currentAddress] = temp_output;
                    }
                    else{
                        if (j == single_frequency){
                            temp_output = ((new_real<<4) & 0xF0) + (new_imaginary & 0x0F);
                        }
                        else{
                            temp_output = ((clipped_offset_default_real<<4) & 0xF0) + (clipped_offset_default_imaginary & 0x0F);
                        }
                        if (XOR_MANUAL){
                            temp_output = temp_output ^ 0x88; //bit flip on sign bit shifts the value by 8: makes unsigned into signed and vice versa.  Currently turns back into signed
                        }
                        packed_data_set[currentAddress] = temp_output;
                    }
                    if (DEBUG_GENERATOR && k == 0)
                        printf("%d ",packed_data_set[currentAddress]);
                }
                if (DEBUG_GENERATOR && k == 0)
                    printf("\n");
            }
        }
    }

    if (DEBUG_GENERATOR)
        printf("END OF DATASET\n");
    return;
}

void time_shift_bins(unsigned char * data,
                     int num_timesteps,
                     int num_frequencies,
                     int num_elements,
                     int num_elements_to_shift,
                     int start_element,
                     int num_time_bins_to_shift,
                     int wrap){

    //the quickest way to do this... Copy a chunk of data (parts that could get overwritten) to a temporary block of memory, then shift the data_block
    //num_time_bins_to_shift < 0 implies that lower times receive larger time values (e.g. -1 implies that t0 = t1, t1 = t2... but high values
    //are undefined or can be wrapped

    //printf("num_elements %d, num_elem_to_shift %d, num_frequencies %d, num_time_bins %d, num_time_bins_to_shift %d\n",num_elements, num_elements_to_shift, num_frequencies, num_timesteps ,num_time_bins_to_shift);
    unsigned char *temp_array = (unsigned char *)calloc(num_elements*num_frequencies*abs(num_time_bins_to_shift),sizeof(unsigned char));
    if (wrap){
        if (num_time_bins_to_shift < 0){
            memcpy(temp_array,data,num_elements*num_frequencies*abs(num_time_bins_to_shift));
        }
        else{
            memcpy(temp_array,&data[(num_timesteps-num_time_bins_to_shift)*num_elements*num_frequencies],num_elements*num_frequencies*num_time_bins_to_shift);
        }
    }
    if (num_time_bins_to_shift > 0){
        for (int k = num_timesteps-1; k >=0; k--){
            //printf("k: %d\n",k);
            for (int j = 0; j < num_frequencies; j++){
                for (int i = start_element; i < start_element + num_elements_to_shift; i++){
                    int from_address = (k-num_time_bins_to_shift)*num_frequencies*num_elements + j*num_elements + i;
                    if (from_address > 0)
                        data[k*num_frequencies*num_elements + j*num_elements + i] = data[from_address];
                    else{
                        from_address = (k)*num_frequencies*num_elements + j*num_elements + i;
                        data[k*num_frequencies*num_elements + j*num_elements + i] = temp_array[from_address];
                    }
                }
            }
        }
    }
    else{
        for (int k = 0; k < num_timesteps; k++){
            //printf("k: %d\n",k);
            for (int j = 0; j < num_frequencies; j++){
                for (int i = start_element; i < start_element + num_elements_to_shift; i++){
                    int from_address = (k-num_time_bins_to_shift)*num_frequencies*num_elements + j*num_elements + i;
                    if (k-num_time_bins_to_shift < num_timesteps)
                        data[k*num_frequencies*num_elements + j*num_elements + i] = data[from_address];
                    else{
                        from_address -= (num_timesteps)*num_frequencies*num_elements;
                        data[k*num_frequencies*num_elements + j*num_elements + i] = temp_array[from_address];
                    }
                }
            }
        }
    }

    free(temp_array);
}

int reorder_data_phaseB_breakData (int num_timesteps,
                                   int num_frequencies,
                                   int num_elements,
                                   unsigned char *packed_data_set){
    //data, when created, has the different elements grouped together.  In Phase B of the test plan, data is not arranged in this manner--
    //data is instead packed such that 8 elements are grouped together for NUM_TIMESAMPLES*NUM_FREQ groups
    //Need to reorganize data to test the kernel that processes data in that format.
    int n_elements_div_2 = num_elements/2;
    unsigned char *packed_data_set2 = (unsigned char*)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    if (packed_data_set2 == NULL){
        printf("Error allocating memory for reordering of data\n");
        return(-1);
    }

    //split data up
    for (int k = 0; k < num_timesteps; k++){
        for (int j = 0; j < num_frequencies; j++){
            for (int i = 0; i < num_elements; i++){
                int inputAddress = k*num_frequencies*num_elements+j*num_elements+i;
                int outputAddress = k*num_frequencies*n_elements_div_2 + j*n_elements_div_2;
                if (i < num_elements/2){
                    outputAddress += i;
                }
                else{
                    outputAddress += num_timesteps*num_frequencies*n_elements_div_2 + i - n_elements_div_2;
                }
                packed_data_set2[outputAddress] = packed_data_set[inputAddress];
            }
        }
    }
    //copy back to original array
    int index = 0;
    for (int k = 0; k < num_timesteps; k++){
        for (int j = 0; j < num_frequencies; j++){
            for (int i = 0; i < num_elements; i++){
                packed_data_set[index] = packed_data_set2[index];
                index++;
            }
        }
    }

    free(packed_data_set2);
    return(0);
}

int reorder_data_interleave_2_frequencies (int num_timesteps,
                                           int num_frequencies,
                                           int num_elements,
                                           int num_data_sets,
                                           unsigned char *packed_data_set){
    //data, when created, has the different elements grouped together.  In this phase of the 16 element correlator
    //the data needs to be interleaved such that F0El0 F1El0 F0El1 F1El0 ... F0El(max-1) F1El(max-1) F2El0 F3El0 F2El1 F3El0 ... F2El(max-1) F3El(max-1)....
    //This function does takes data F0El0 F0El1 ... F0El(max-1) F1El0 F1El1 ... F1El(max-1) ....

    unsigned char *packed_data_set2 = (unsigned char*)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    if (packed_data_set2 == NULL){
        printf("Error allocating memory for reordering of data\n");
        return(-1);
    }

    //split data up
    int outputAddress = 0;
    for (int m = 0; m < num_data_sets; m++){
        for (int k = 0; k < num_timesteps; k++){
            for (int j = 0; j < num_frequencies/2; j++){
                for (int i = 0; i < num_elements; i++){
                    int inputAddress = m*num_timesteps*num_frequencies*num_elements + k*num_frequencies*num_elements+j*num_elements*2+i;
                    packed_data_set2[outputAddress++] = packed_data_set[inputAddress];
                    inputAddress += num_elements;
                    packed_data_set2[outputAddress++] = packed_data_set[inputAddress];
                }
            }
        }
    }
    //copy back to original array
    int index = 0;
    for (int m = 0; m < num_data_sets; m++){
        for (int k = 0; k < num_timesteps; k++){
            for (int j = 0; j < num_frequencies; j++){
                for (int i = 0; i < num_elements; i++){
                    packed_data_set[index] = packed_data_set2[index];
                    index++;
                }
            }
        }
    }

    free(packed_data_set2);
    return(0);
}

int drop_packets_pseudo_random(double drop_fraction,
                               int packet_size,
                               int drop_value,
                               int drop_seed,
                               int array_length,
                               unsigned char *packed_data_set){

    unsigned char *drop_vals = (unsigned char *)malloc(packet_size*sizeof(unsigned char));
    if (drop_vals == NULL){
        printf("error allocating memory in drop_packets_pseudo_random\n");
        return (-1);
    }

    drop_value = offset_and_clip_value(drop_value, 8, 0, 15);
    unsigned char clipped_offset_drop_value = (unsigned char) drop_value;
    unsigned char unsigned_packed_drop_val = ((clipped_offset_drop_value<<4) & 0xF0) + (clipped_offset_drop_value & 0x0F);
    memset(drop_vals,unsigned_packed_drop_val,packet_size);

    //set pseudorandom number set
    srand(drop_seed);
    if (drop_fraction != 0){
        int one_over_drop_fraction = (int) (1./drop_fraction);
        for (int i = 0; i < array_length/packet_size; i++){
            if (rand() % one_over_drop_fraction == 1){
                memcpy(&packed_data_set[i*packet_size],drop_vals,packet_size);
            }
        }
    }
    return (0);
}



void print_element_data(int num_timesteps, int num_frequencies, int num_elements, int particular_frequency, unsigned char *data){
    printf("Number of timesteps to print: %d, ", num_timesteps);
    if (particular_frequency == ALL_FREQUENCIES)
        printf("number of frequency bands: %d, number of elements: %d\n", num_frequencies, num_elements);
    else
        printf("frequency band: %d, number of elements: %d\n", particular_frequency, num_elements);

    for (int k = 0; k < num_timesteps; k++){
        if (num_timesteps > 1){
            printf("Time Step %d\n", k);
        }
        printf("            ");
        for (int header_i = 0; header_i < num_elements; header_i++){
            printf("%3dR %3dI ", header_i, header_i);
        }
        printf("\n");
        for (int j = 0; j < num_frequencies; j++){
            if (particular_frequency == ALL_FREQUENCIES || particular_frequency == j){
                if (particular_frequency == j)
                    printf("Freq: %4d: ", j);

                for (int i = 0; i < num_elements; i++){
                    unsigned char temp = data[k*num_frequencies*num_elements+j*num_elements+i];
                    if (XOR_MANUAL){
                        temp = temp ^ 0x88;
                    }
                    printf("%4d %4d ",(int)(HI_NIBBLE(temp))-8,(int)(LO_NIBBLE(temp))-8);
                }
                printf("\n");
            }
        }
    }
    printf("\n");
}

int cpu_data_generate_and_correlate(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*num_data_sets*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, num_data_sets, generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    for (int m = 0; m < num_data_sets; m++){
        for (int k = 0; k < num_timesteps; k++){
            for (int j = 0; j < num_frequencies; j++){
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[m*num_timesteps*num_frequencies*num_elements + k*num_frequencies*num_elements + j*num_elements + element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = 0; element_x < num_elements; element_x++){
                        temp_char = generated[m*num_timesteps*num_frequencies*num_elements + k*num_frequencies*num_elements + j*num_elements + element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
                            correlated_data[(m*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[(m*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                        else{
                            correlated_data[(m*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[(m*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
        }
    }
    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_gated(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data, int time_offset_remainder, int gate_period){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements,1, generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    int time_for_bin_calc = time_offset_remainder;
    int time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = 0; element_x < num_elements; element_x++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period)
            time_for_bin_calc -= num_data_sets*gate_period;
    }

    //clean up parameters as needed
    free(generated);
    return (0);
}


int cpu_data_generate_and_correlate_gated_3_ways(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data, int time_offset_remainder, int gate_period, int time_offset_remainder2, int gate_period2, int time_offset_remainder3, int gate_period3){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements,1, generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    int time_for_bin_calc = time_offset_remainder;
    int time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = 0; element_x < num_elements; element_x++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period)
            time_for_bin_calc -= num_data_sets*gate_period;
    }

    ///
    time_for_bin_calc = time_offset_remainder2;
    time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period2;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = 0; element_x < num_elements; element_x++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period2)
            time_for_bin_calc -= num_data_sets*gate_period2;
    }

    ///
    time_for_bin_calc = time_offset_remainder3;
    time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period3;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = 0; element_x < num_elements; element_x++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements*2 + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements*2 + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements*2 + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data[(num_data_sets*num_frequencies*num_elements*num_elements*2 + time_bin*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period3)
            time_for_bin_calc -= num_data_sets*gate_period3;
    }
    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_gated_general(int num_timesteps, int num_frequencies, int num_elements, unsigned int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements,1, generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    for (int g_group = 0; g_group < num_gate_groups; g_group++){
        int time_for_bin_calc = time_offset_remainder[g_group];
        int time_counter = 0;
        printf("time_offset_remainder[%d]= %d\n",g_group,time_offset_remainder[g_group]);
        printf("single_gate_period[%d]= %d\n",g_group,single_gate_period[g_group]);
        for (int k = 0; k < num_timesteps; k++){
            int time_bin = time_for_bin_calc/single_gate_period[g_group];
            int reduced_time_bin = (time_bin >= bin_cutoff[g_group]) ? 1 : 0;
            for (int j = 0; j < num_frequencies; j++){
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = 0; element_x < num_elements; element_x++){
                        temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
                            correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                        else{
                            correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
            time_counter++;
            if (time_counter == BASE_TIMESAMPLES_ACCUM){
                time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
                time_counter = 0;
            }
            if (time_for_bin_calc >= num_time_bins[g_group]*single_gate_period[g_group])
                time_for_bin_calc -= num_time_bins[g_group]*single_gate_period[g_group];
        }
    }

    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_gated_general_time_shifting(int num_timesteps, int num_frequencies, int num_elements, unsigned int *zone_mask_lookup, int *correlated_data, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period,int num_elements_to_shift, int num_start_element, int num_time_bins_to_shift, int wrap){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements,1, generated);

//     void time_shift_bins(unsigned char * data,
//                      int num_timesteps,
//                      int num_frequencies,
//                      int num_elements,
//                      int num_elements_to_shift,
//                      int start_element,
//                      int num_time_bins_to_shift,
//                      int wrap){
    time_shift_bins(generated,num_timesteps, num_frequencies,num_elements,num_elements_to_shift, num_start_element,num_time_bins_to_shift,wrap);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    for (int g_group = 0; g_group < num_gate_groups; g_group++){
        int time_for_bin_calc = time_offset_remainder[g_group];
        int time_counter = 0;
        printf("time_offset_remainder[%d]= %d\n",g_group,time_offset_remainder[g_group]);
        printf("single_gate_period[%d]= %d\n",g_group,single_gate_period[g_group]);
        for (int k = 0; k < num_timesteps; k++){
            int time_bin = time_for_bin_calc %(NUM_TIME_BINS*single_gate_period[g_group]);
            time_bin = time_for_bin_calc/single_gate_period[g_group];
            //int time_bin = time_for_bin_calc/single_gate_period[g_group];
            //int reduced_time_bin = (time_bin >= bin_cutoff[g_group]) ? 1 : 0;
            for (int j = 0; j < num_frequencies; j++){
                int output_zone = zone_mask_lookup[g_group*NUM_TIME_BINS*num_frequencies+j*NUM_TIME_BINS+time_bin];
                int output_counter = g_group*NUM_ZONES*num_frequencies*num_elements*num_elements*2+
                                        j*NUM_ZONES*num_elements*num_elements*2 +
                                        output_zone*num_elements*num_elements*2;
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = 0; element_x < num_elements; element_x++){
                        temp_char = generated[ k*num_frequencies*num_elements + j*num_elements + element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
//                             correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2]   += element_x_re*element_y_re + element_x_im*element_y_im;
//                             correlated_data[((reduced_time_bin+2*g_group)*num_frequencies*num_elements*num_elements + j*num_elements*num_elements+element_y*num_elements+element_x)*2+1] += element_x_im*element_y_re - element_x_re*element_y_im;
                            correlated_data[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;

                        }
                        else{
                            correlated_data[output_counter++]  = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data[output_counter++]  = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
            time_counter++;
            if (time_counter == BASE_TIMESAMPLES_ACCUM){
                time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
                time_counter = 0;
            }
            if (time_for_bin_calc >= NUM_TIME_BINS*single_gate_period[g_group])
                time_for_bin_calc -= NUM_TIME_BINS*single_gate_period[g_group];
        }
    }

    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_upper_triangle_only(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data_triangle){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*num_data_sets*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, num_data_sets,generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    for (int m = 0; m < num_data_sets; m++){
        for (int k = 0; k < num_timesteps; k++){
            int output_counter = m*num_frequencies*num_elements*(num_elements+1);// /2*2;
            for (int j = 0; j < num_frequencies; j++){
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[m*num_timesteps*num_frequencies*num_elements + k*num_frequencies*num_elements+j*num_elements+element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = element_y; element_x < num_elements; element_x++){
                        temp_char = generated[m*num_timesteps*num_frequencies*num_elements + k*num_frequencies*num_elements+j*num_elements+element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
                            correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                        else{
                            correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
        }
    }

    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_upper_triangle_only_gated(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data_triangle, int time_offset_remainder, int gate_period){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, 1,generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }

    unsigned char temp_char;
    //correlate based on generated data
    int time_for_bin_calc = time_offset_remainder;
    int time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period;
        int output_counter = time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = element_y; element_x < num_elements; element_x++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period)
            time_for_bin_calc -= (num_data_sets*gate_period);
    }

    //clean up parameters as needed
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_upper_triangle_only_gated_3_ways(int num_timesteps, int num_frequencies, int num_elements, int num_data_sets, int *correlated_data_triangle, int time_offset_remainder, int gate_period, int time_offset_remainder2, int gate_period2, int time_offset_remainder3, int gate_period3){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, 1,generated);

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }
    printf("started1\n");
    unsigned char temp_char;
    //correlate based on generated data
    int time_for_bin_calc = time_offset_remainder;
    int time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period;
        int output_counter = time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = element_y; element_x < num_elements; element_x++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period)
            time_for_bin_calc -= (num_data_sets*gate_period);
    }
    printf("done1\n");
    ///
    time_for_bin_calc = time_offset_remainder2;
    time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period2;
        int output_counter = num_data_sets*num_frequencies*num_elements*(num_elements+1) + time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = element_y; element_x < num_elements; element_x++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period2)
            time_for_bin_calc -= (num_data_sets*gate_period2);
    }
    printf("done2\n");
    ///
    time_for_bin_calc = time_offset_remainder3;
    time_counter = 0;
    for (int k = 0; k < num_timesteps; k++){
        int time_bin = time_for_bin_calc/gate_period3;
        int output_counter = num_data_sets*num_frequencies*num_elements*(num_elements+1)*2 + time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
        for (int j = 0; j < num_frequencies; j++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                if (XOR_MANUAL){
                    temp_char = temp_char ^ 0x88;
                }
                int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                for (int element_x = element_y; element_x < num_elements; element_x++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                    int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    if (k != 0){
                        correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                    else{
                        correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                        correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                    }
                }
            }
        }
        time_counter++;
        if (time_counter == BASE_TIMESAMPLES_ACCUM){
            time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
            time_counter = 0;
        }
        if (time_for_bin_calc >= num_data_sets*gate_period3)
            time_for_bin_calc -= (num_data_sets*gate_period3);
    }
    //clean up parameters as needed
    printf("done3\n");
    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_upper_triangle_only_gated_general(int num_timesteps, int num_frequencies, int num_elements, unsigned int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data_triangle, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, 1,generated);

    //if there is a time shift for bins to be had, it should go here.

    //

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }
    //printf("started1\n");
    unsigned char temp_char;
    //correlate based on generated data
    for (int g_group = 0; g_group < num_gate_groups; g_group++){
        int time_for_bin_calc = time_offset_remainder[g_group];
        int time_counter = 0;
        printf("time_offset_remainder[%d]= %d\n",g_group,time_offset_remainder[g_group]);
        printf("single_gate_period[%d]= %d\n",g_group,single_gate_period[g_group]);
        for (int k = 0; k < num_timesteps; k++){
            int time_bin = time_for_bin_calc/single_gate_period[g_group];
            int reduced_time_bin = (time_bin >= bin_cutoff[g_group]) ? 1 : 0;
            int output_counter = g_group*num_frequencies*num_elements*(num_elements+1)*2 + reduced_time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
            for (int j = 0; j < num_frequencies; j++){
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = element_y; element_x < num_elements; element_x++){
                        temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
                            correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                        else{
                            correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
            time_counter++;
            if (time_counter == BASE_TIMESAMPLES_ACCUM){
                time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
                time_counter = 0;
            }
            if (time_for_bin_calc >= num_time_bins[g_group]*single_gate_period[g_group])
                time_for_bin_calc -= (num_time_bins[g_group]*single_gate_period[g_group]);
        }
        printf("Done stage %d \n",g_group);
    }

    free(generated);
    return (0);
}

int cpu_data_generate_and_correlate_upper_triangle_only_gated_general_time_shifting(int num_timesteps, int num_frequencies, int num_elements, unsigned int *zone_mask_lookup, int *correlated_data_triangle, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period,int num_elements_to_shift, int num_start_element, int num_time_bins_to_shift, int wrap){
    //correlatedData will be returned as num_frequencies blocks, each num_elements x num_elements x 2

    //generate a dataset that should be the same as what the gpu is testing
    //dataset will be num_timesteps x num_frequencies x num_elements large
    unsigned char *generated = (unsigned char *)malloc(num_timesteps*num_frequencies*num_elements*sizeof(unsigned char));
    //check the array was allocated properly
    if (generated == NULL){
        printf ("Error allocating memory: cpu_data_generate_and_correlate\n");
        return (-1);
    }

    generate_char_data_set(GEN_TYPE,GEN_DEFAULT_SEED,GEN_DEFAULT_RE, GEN_DEFAULT_IM,GEN_INITIAL_RE,GEN_INITIAL_IM,GEN_FREQ, num_timesteps, num_frequencies, num_elements, 1,generated);

    //if there is a time shift for bins to be had, it should go here.
    time_shift_bins(generated,num_timesteps, num_frequencies,num_elements,num_elements_to_shift, num_start_element,num_time_bins_to_shift,wrap);
    //

    if (CHECKING_VERBOSE){
        print_element_data(1, num_frequencies, num_elements, ALL_FREQUENCIES, generated);
    }
    //printf("started1\n");
    unsigned char temp_char;
    //correlate based on generated data
    for (int g_group = 0; g_group < num_gate_groups; g_group++){
        int time_for_bin_calc = time_offset_remainder[g_group];
        int time_counter = 0;
        printf("time_offset_remainder[%d]= %d\n",g_group,time_offset_remainder[g_group]);
        printf("single_gate_period[%d]= %d\n",g_group,single_gate_period[g_group]);
        for (int k = 0; k < num_timesteps; k++){
            int time_bin = time_for_bin_calc %(NUM_TIME_BINS*single_gate_period[g_group]);
            time_bin = time_for_bin_calc/single_gate_period[g_group];
            //move the next two lines...
            //int reduced_time_bin = (time_bin >= bin_cutoff[g_group]) ? 1 : 0;
            //int output_counter = g_group*num_frequencies*num_elements*(num_elements+1)*2 + reduced_time_bin*num_frequencies*num_elements*(num_elements+1);// /2*2;
            for (int j = 0; j < num_frequencies; j++){
                int output_zone = zone_mask_lookup[g_group*NUM_TIME_BINS*num_frequencies+j*NUM_TIME_BINS+time_bin];
                //TODO Check this next expression NUM_DATA_SETS
                int output_counter = g_group*NUM_ZONES*num_frequencies*num_elements*(num_elements+1)/2*2+
                                        j*num_elements*(num_elements+1)/2*2 +
                                        output_zone*num_frequencies*num_elements*(num_elements+1)/2*2;
                for (int element_y = 0; element_y < num_elements; element_y++){
                    temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_y];
                    if (XOR_MANUAL){
                        temp_char = temp_char ^ 0x88;
                    }
                    int element_y_re = (int)(HI_NIBBLE(temp_char)) - 8; //-8 is to put the number back in the range -8 to 7 from 0 to 15
                    int element_y_im = (int)(LO_NIBBLE(temp_char)) - 8;
                    for (int element_x = element_y; element_x < num_elements; element_x++){
                        temp_char = generated[k*num_frequencies*num_elements+j*num_elements+element_x];
                        if (XOR_MANUAL){
                            temp_char = temp_char ^ 0x88;
                        }
                        int element_x_re = (int)(HI_NIBBLE(temp_char)) - 8;
                        int element_x_im = (int)(LO_NIBBLE(temp_char)) - 8;
                        if (k != 0){
                            correlated_data_triangle[output_counter++] += element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] += element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                        else{
                            correlated_data_triangle[output_counter++] = element_x_re*element_y_re + element_x_im*element_y_im;
                            correlated_data_triangle[output_counter++] = element_x_im*element_y_re - element_x_re*element_y_im;
                        }
                    }
                }
            }
            time_counter++;
            if (time_counter == BASE_TIMESAMPLES_ACCUM){
                time_for_bin_calc += TIME_FOR_TIMESTEP_IN_10ns_UNITS*BASE_TIMESAMPLES_ACCUM;
                time_counter = 0;
            }
            if (time_for_bin_calc >= NUM_TIME_BINS*single_gate_period[g_group])
                time_for_bin_calc -= (NUM_TIME_BINS*single_gate_period[g_group]);
        }
        printf("Done stage %d \n",g_group);
    }

    free(generated);
    return (0);
}

void reorganize_32_to_16_feed_GPU_Correlated_Data(int actual_num_frequencies, int actual_num_elements, int num_data_sets, int *correlated_data){
    //data is processed as 32 elements x 32 elements to fit the kernel even though only 16 elements exist.
    //This is equivalent to processing 2 elements at the same time, where the desired correlations live in the first and fourth quadrants
    //This function is to reorganize the data so that comparisons can be done more easily

    //The input dataset is larger than the output, so can reorganize in the same array

    int input_frequencies = actual_num_frequencies/2;
    int input_elements = actual_num_elements*2;
    int address = 0;
    int address_out = 0;
    for (int m = 0; m < num_data_sets; m++){
        for (int freq = 0; freq < input_frequencies; freq++){
            for (int element_y = 0; element_y < input_elements; element_y++){
                for (int element_x = 0; element_x < input_elements; element_x++){
                    if (element_x < actual_num_elements && element_y < actual_num_elements){
                        correlated_data[address_out++] = correlated_data[address++];
                        correlated_data[address_out++] = correlated_data[address++]; //real and imaginary at each spot
                    }
                    else if (element_x >=actual_num_elements && element_y >=actual_num_elements){
                        correlated_data[address_out++] = correlated_data[address++];
                        correlated_data[address_out++] = correlated_data[address++];
                    }
                    else
                        address += 2;
                }
            }
        }
    }
    return;
}

void reorganize_32_to_16_feed_GPU_Correlated_Data_Interleaved(int actual_num_frequencies, int actual_num_elements, int num_data_sets, int *correlated_data){
    //data is processed as 32 elements x 32 elements to fit the kernel even though only 16 elements exist.
    //There are two frequencies interleaved...  Need to sort the output values properly
    //This function is to reorganize the data so that comparisons can be done more easily

    //The input dataset is larger than the output, so can reorganize in the same array
    int *temp_output = (int *)malloc(actual_num_elements*actual_num_elements*actual_num_frequencies*num_data_sets*2*sizeof(int));

    int input_elements = actual_num_elements*2;
    int address = 0;
    int address_out = 0;
    for (int m = 0; m < num_data_sets; m++){
        for (int freq = 0; freq < actual_num_frequencies; freq++){
            address = m*actual_num_frequencies*input_elements*input_elements*2 + (freq >>1) * input_elements*input_elements *2; // freq>>1 == freq/2
            for (int element_y = 0; element_y < input_elements; element_y++){
                for (int element_x = 0; element_x < input_elements; element_x++){
                    if (freq & 1){//odd frequencies
                        if ((element_x & 1) && (element_y & 1)){
                            temp_output[address_out++] = correlated_data[address++];
                            temp_output[address_out++] = correlated_data[address++];
                        }
                        else{
                            address += 2;
                        }
                    }
                    else{ // even frequencies
                        if ((!(element_x & 1)) && (!(element_y & 1))){
                            temp_output[address_out++] = correlated_data[address++];
                            temp_output[address_out++] = correlated_data[address++];
                        }
                        else{
                            address += 2;
                        }
                    }
                }
            }
        }
    }

    //copy the results back into correlated_data
    for (int i = 0; i < actual_num_frequencies * actual_num_elements*actual_num_elements * num_data_sets * 2; i++)
        correlated_data[i] = temp_output[i];

    free(temp_output);
    return;
}


void reorganize_GPU_to_full_Matrix_for_comparison(int block_side_length, int num_blocks, int actual_num_frequencies, int actual_num_elements, int num_data_sets, int *gpu_data, int *final_matrix){
    //takes the output data, grouped in blocks of block_dim x block_dim x 2 (complex pairs (ReIm)of ints), and fills a num_elements x num_elements x 2

    for (int m = 0; m < num_data_sets; m++){
        for (int frequency_bin = 0; frequency_bin < actual_num_frequencies; frequency_bin++ ){
            int block_x_ID = 0;
            int block_y_ID = 0;
            int num_blocks_x = actual_num_elements/block_side_length;
            int block_check = num_blocks_x;

            for (int block_ID = 0; block_ID < num_blocks; block_ID++){
                if (block_ID == block_check){ //at the end of a row in the upper triangle
                    num_blocks_x--;
                    block_check += num_blocks_x;
                    block_y_ID++;
                    block_x_ID = block_y_ID;
                }
                for (int y_ID_local = 0; y_ID_local < block_side_length; y_ID_local++){
                    int y_ID_global = block_y_ID * block_side_length + y_ID_local;
                    for (int x_ID_local = 0; x_ID_local < block_side_length; x_ID_local++){
                        int GPU_address = m*actual_num_frequencies*num_blocks*block_side_length*block_side_length*2 + frequency_bin*(num_blocks*block_side_length*block_side_length*2) + block_ID *(block_side_length*block_side_length*2) + y_ID_local*block_side_length*2+x_ID_local*2; ///TO DO :simplify this statement after getting everything working
                        int x_ID_global = block_x_ID * block_side_length + x_ID_local;
                        if (x_ID_global >= y_ID_global){
                            if (x_ID_global > y_ID_global){ //store the conjugate: x and y addresses get swapped and the imaginary value is the negative of the original value
                                final_matrix[(m*actual_num_frequencies*actual_num_elements*actual_num_elements +frequency_bin*actual_num_elements*actual_num_elements+x_ID_global*actual_num_elements+y_ID_global)*2]   =  gpu_data[GPU_address];
                                final_matrix[(m*actual_num_frequencies*actual_num_elements*actual_num_elements +frequency_bin*actual_num_elements*actual_num_elements+x_ID_global*actual_num_elements+y_ID_global)*2+1] = -gpu_data[GPU_address+1];
                            }
                            //store the value for the upper triangle
                            final_matrix[(m*actual_num_frequencies*actual_num_elements*actual_num_elements +frequency_bin*actual_num_elements*actual_num_elements+y_ID_global*actual_num_elements+x_ID_global)*2]   = gpu_data[GPU_address];
                            final_matrix[(m*actual_num_frequencies*actual_num_elements*actual_num_elements +frequency_bin*actual_num_elements*actual_num_elements+y_ID_global*actual_num_elements+x_ID_global)*2+1] = gpu_data[GPU_address+1];
                        }
                    }
                }
                //printf("block_ID: %d, block_y_ID: %d, block_x_ID: %d\n", block_ID, block_y_ID, block_x_ID);
                //update block offset values
                block_x_ID++;
            }
        }
    }
    return;
}

void reorganize_GPU_to_upper_triangle(int block_side_length, int num_blocks, int actual_num_frequencies, int actual_num_elements, int num_data_sets, int *gpu_data, int *final_matrix){

    for (int m = 0; m < num_data_sets; m++){
        int GPU_address = m*(actual_num_frequencies*(num_blocks*(block_side_length*block_side_length*2))); //we go through the gpu data sequentially and map it to the proper locations in the output array
        for (int frequency_bin = 0; frequency_bin < actual_num_frequencies; frequency_bin++ ){
            int block_x_ID = 0;
            int block_y_ID = 0;
            int num_blocks_x = actual_num_elements/block_side_length;
            int block_check = num_blocks_x;
            int frequency_offset = m*actual_num_frequencies*(actual_num_elements* (actual_num_elements+1))/2 + frequency_bin * (actual_num_elements* (actual_num_elements+1))/2;// frequency_bin * number of items in an upper triangle

            for (int block_ID = 0; block_ID < num_blocks; block_ID++){
                if (block_ID == block_check){ //at the end of a row in the upper triangle
                    num_blocks_x--;
                    block_check += num_blocks_x;
                    block_y_ID++;
                    block_x_ID = block_y_ID;
                }

                for (int y_ID_local = 0; y_ID_local < block_side_length; y_ID_local++){

                    for (int x_ID_local = 0; x_ID_local < block_side_length; x_ID_local++){

                        int x_ID_global = block_x_ID * block_side_length + x_ID_local;
                        int y_ID_global = block_y_ID * block_side_length + y_ID_local;

                        /// address_1d_output = frequency_offset, plus the number of entries in the rectangle area (y_ID_global*actual_num_elements), minus the number of elements in lower triangle to that row (((y_ID_global-1)*y_ID_global)/2), plus the contributions to the address from the current row (x_ID_global - y_ID_global)
                        int address_1d_output = frequency_offset + y_ID_global*actual_num_elements - ((y_ID_global-1)*y_ID_global)/2 + (x_ID_global - y_ID_global);

                        if (block_x_ID != block_y_ID){ //when we are not in the diagonal blocks
                            final_matrix[address_1d_output*2  ] = gpu_data[GPU_address++];
                            final_matrix[address_1d_output*2+1] = gpu_data[GPU_address++];
                        }
                        else{ // the special case needed to deal with the diagonal pieces
                            if (x_ID_local >= y_ID_local){
                                final_matrix[address_1d_output*2  ] = gpu_data[GPU_address++];
                                final_matrix[address_1d_output*2+1] = gpu_data[GPU_address++];
                            }
                            else{
                                GPU_address += 2;
                            }
                        }
                    }
                }
                //offset_GPU += (block_side_length*block_side_length);
                //update block offset values
                block_x_ID++;
            }
        }
    }
    return;
}

void shuffle_data_to_frequency_major_output (int num_frequencies_final, int num_frequencies, int num_elements, int num_data_sets, int *input_data, double complex *output_data){
    //input data should be arranged as (num_elements*(num_elements+1))/2 (real,imag) pairs of complex visibilities for frequencies
    //output array will be sparsely to moderately filled, so loop such that writing is done in sequential order
    int num_complex_visibilities = (num_elements*(num_elements+1))/2;
    for (int m = 0; m < num_data_sets; m++){
        int output_counter = m*num_frequencies_final*num_complex_visibilities;
        for (int data_count = 0; data_count < num_complex_visibilities; data_count++){
            for (int freq_count = 0; freq_count < num_frequencies_final; freq_count++){
                if (freq_count < num_frequencies){
                    output_data [output_counter++] = (double)input_data[(m*num_frequencies*num_complex_visibilities +freq_count*num_complex_visibilities + data_count)*2]
                                                    + I * (double)input_data[(m*num_frequencies*num_complex_visibilities +freq_count*num_complex_visibilities + data_count)*2+1];
                    //output_data [(data_count*num_frequencies_final + freq_count)] = (double)input_data[(freq_count*num_complex_visibilities + data_count)*2] + I * (double)input_data[(freq_count*num_complex_visibilities + data_count)*2+1];
                }
                else{
                    output_data [output_counter++] = (double)0.0 + I * (double)0.0;
                    //output_data [(data_count*num_frequencies_final + freq_count)] = (double)0.0 + I * (double)0.0;
                }
            }
        }
    }
    return;
}

void shuffle_data_to_frequency_major_output_16_element_with_triangle_conversion (int num_frequencies_final, int actual_num_frequencies, int num_data_sets, int *input_data, double complex *output_data){
    //input data should be arranged as (num_elements*(num_elements+1))/2 (real,imag) pairs of complex visibilities for frequencies
    //output array will be sparsely to moderately filled, so loop such that writing is done in sequential order
    //int num_complex_visibilities = 136;//16*(16+1)/2; //(n*(n+1)/2)
    for (int m = 0; m < num_data_sets; m++){
        int output_counter = m*num_frequencies_final*136;//16*(16+1)/2 = 136
        for (int y = 0; y < 16; y++){
            for (int x = y; x < 16; x++){
                for (int freq_count = 0; freq_count < num_frequencies_final; freq_count++){
                    if (freq_count < actual_num_frequencies){
                        int input_index = (m*actual_num_frequencies*256 + freq_count*256 + y*16 + x)*2; //num of array elements in a block: 16*16 = 256 if you are curious where that came from
                        output_data [output_counter++] = (double) input_data[input_index]
                                                        + I * (double) input_data[input_index+1];
                        //output_data [(data_count*num_frequencies_final + freq_count)] = (double)input_data[input_index] + I * (double)input_data[input_index+1];
                    }
                    else{
                        output_data [output_counter++] = (double)0.0 + I * (double)0.0;
                        //output_data [(data_count*num_frequencies_final + freq_count)] = (double)0.0 + I * (double)0.0;
                    }
                }
            }
        }
    }
    return;
}

void reorganize_data_16_element_with_triangle_conversion (int num_frequencies_final, int actual_num_frequencies, int num_data_sets, int *input_data, int *output_data){
    //input data should be arranged as (num_elements*(num_elements+1))/2 (real,imag) pairs of complex visibilities for frequencies
    //output array will be sparsely to moderately filled, so loop such that writing is done in sequential order
    for (int m = 0; m < num_data_sets; m++){
        int output_counter = m * num_frequencies_final * 136 *2;
        for (int freq_count = 0; freq_count < num_frequencies_final; freq_count++){
            for (int y = 0; y < 16; y++){
                for (int x = y; x < 16; x++){
                    if (freq_count < actual_num_frequencies){
                        int input_index = (m*actual_num_frequencies*256 + freq_count*256 + y*16 + x)*2; //blocks of data are 16 x 16 = 256 and row_stride is 16
                        output_data [output_counter++] = input_data[input_index];
                        output_data [output_counter++] = input_data[input_index+1];
                        //output_data [(data_count*num_frequencies_final + freq_count)] = (double)input_data[input_index] + I * (double)input_data[input_index+1];
                    }
                    else{
                        output_data [output_counter++] = 0;
                        output_data [output_counter++] = 0;
                        //output_data [(data_count*num_frequencies_final + freq_count)] = (double)0.0 + I * (double)0.0;
                    }
                }
            }
        }
    }
    return;
}

void correct_GPU_correlation_results (int num_timesteps, int num_frequencies, int num_elements, int *correlated_data_GPU, int *accumulates){
    //since data are processed within the GPU as unsigned values (packing to optimize calculations) correction terms must be taken account of
    //Future versions of kernels will automatically correct on the GPU so this function will not be required then
    int address = 0;
    int offset = num_timesteps * 128;
    for (int freq = 0; freq < num_frequencies; freq++){
        for (int element_y = 0; element_y < num_elements; element_y++){
            int element_y_re = accumulates[(freq*num_elements+element_y)*2];
            int element_y_im = accumulates[(freq*num_elements+element_y)*2+1];
            for (int element_x = 0; element_x < num_elements; element_x++){
                int element_x_re = accumulates[(freq*num_elements+element_x)*2];
                int element_x_im = accumulates[(freq*num_elements+element_x)*2+1];
                correlated_data_GPU[address++] += offset - 8 * (element_x_re + element_x_im + element_y_re + element_y_im);
                correlated_data_GPU[address++] += 8*(element_x_re-element_x_im-element_y_re+element_y_im);
            }
        }
    }
    return;
}

void correct_GPU_correlation_results_Split (int num_timesteps, int num_frequencies, int num_elements, int *correlated_data_GPU, int *accumulates){
    //since data are processed within the GPU as unsigned values (packing to optimize calculations) correction terms must be taken account of
    //Future versions of kernels will automatically correct on the GPU so this function will not be required then
    int address = 0;
    int offset = num_timesteps * 128;
    for (int freq = 0; freq < num_frequencies; freq++){
        for (int element_y = 0; element_y < num_elements; element_y++){
            int y_addr = freq*num_elements/2;
            if (element_y < num_elements/2){
                y_addr += element_y;
            }
            else{
                y_addr += num_frequencies*num_elements/2 + element_y - num_elements/2;
            }
            int element_y_re = accumulates[y_addr*2];
            int element_y_im = accumulates[y_addr*2+1];
            for (int element_x = 0; element_x < num_elements; element_x++){
                int x_addr = freq*num_elements/2;
                if (element_x < num_elements/2){
                    x_addr += element_x;
                }
                else{
                    x_addr += num_frequencies*num_elements/2 + element_x - num_elements/2;
                }
                int element_x_re = accumulates[x_addr*2];
                int element_x_im = accumulates[x_addr*2+1];
                correlated_data_GPU[address++] += offset - 8 * (element_x_re + element_x_im + element_y_re + element_y_im);
                correlated_data_GPU[address++] += 8*(element_x_re-element_x_im-element_y_re+element_y_im);
            }
        }
    }
    return;
}

void compare_NSquared_correlator_results ( int *num_err, int64_t *err_2, int num_frequencies, int num_elements, int num_data_sets, int *data_set_GPU, int *data_set_CPU, double *ratio_GPU_div_CPU, double *phase_difference, int verbosity){
    //this will compare the values of the two arrays and give information about the comparison
    int address = 0;
    int local_Address = 0;
    *num_err = 0;
    *err_2 = 0;
    int max_error = 0;
    int amplitude_squared_error;
    double amplitude_squared_CPU;
    double amplitude_squared_GPU;
    double phase_angle_CPU;
    double phase_angle_GPU;
    for (int m = 0; m < num_data_sets; m++){
        for (int freq = 0; freq < num_frequencies; freq++){
            for (int element_y = 0; element_y < num_elements; element_y++){
                for (int element_x = 0; element_x < num_elements; element_x++){
                    //compare real results
                    int data_Real_GPU = data_set_GPU[address];
                    int data_Real_CPU = data_set_CPU[address++];
                    int difference_real = data_Real_GPU - data_Real_CPU;
                    //compare imaginary results
                    int data_Imag_GPU = data_set_GPU[address];
                    int data_Imag_CPU = data_set_CPU[address++];
                    int difference_imag = data_Imag_GPU - data_Imag_CPU;

                    //get amplitude_squared
                    amplitude_squared_CPU = data_Real_CPU*data_Real_CPU + data_Imag_CPU*data_Imag_CPU;
                    amplitude_squared_GPU = data_Real_GPU*data_Real_GPU + data_Imag_GPU*data_Imag_GPU;
                    phase_angle_CPU = atan2((double)data_Imag_CPU,(double)data_Real_CPU);
                    phase_angle_GPU = atan2((double)data_Imag_GPU,(double)data_Real_GPU);

                    if (amplitude_squared_CPU != 0){
                        ratio_GPU_div_CPU[local_Address] = amplitude_squared_GPU/amplitude_squared_CPU;
                    }
                    else{
                        ratio_GPU_div_CPU[local_Address] = -1;//amplitude_squared_GPU/amplitude_squared_CPU;
                    }

                    phase_difference[local_Address++] = phase_angle_GPU - phase_angle_CPU;

                    if (difference_real != 0 || difference_imag !=0){
                        (*num_err)++;
                        if (verbosity ){
                            printf ("freq: %6d element_x: %6d element_y: %6d Real CPU/GPU %8d %8d Imaginary CPU/GPU %8d %8d ERR: %7d\n",freq, element_x, element_y, data_Real_CPU, data_Real_GPU, data_Imag_CPU, data_Imag_GPU, *num_err);
                        }
                        amplitude_squared_error = difference_imag*difference_imag+difference_real*difference_real;
                        *err_2 += amplitude_squared_error;
                        if (amplitude_squared_error > max_error)
                            max_error = amplitude_squared_error;
                    }
                    else{
                        if (verbosity){
                            printf ("freq: %6d element_x: %6d element_y: %6d Real CPU/GPU %8d %8d Imaginary CPU/GPU %8d %8d\n",freq, element_x, element_y, data_Real_CPU, data_Real_GPU, data_Imag_CPU, data_Imag_GPU);
                        }
                    }
                }
            }
        }
    }
    printf("\nTotal number of errors: %d, Sum of Squared Differences: %lld \n",*num_err, (long long int) *err_2);
    printf("sqrt(sum of squared differences/numberElements): %f \n", sqrt((*err_2)*1.0/local_Address));//add some more data--find maximum error, figure out other statistical properties
    printf("Maximum amplitude squared error: %d\n", max_error);
    return;
}

void compare_NSquared_correlator_results_data_has_upper_triangle_only ( int *num_err, int64_t *err_2, int total_output_frequencies, int actual_num_frequencies, int actual_num_elements, int num_data_sets,int *data_set_GPU, int *data_set_CPU, double *ratio_GPU_div_CPU, double *phase_difference, int verbosity){
    //this will compare the values of the two arrays and give information about the comparison
    int address = 0;
    int addressGPU = 0;
    int local_Address = 0;
    *num_err = 0;
    *err_2 = 0;
    int max_error = 0;
    int amplitude_squared_error;
    double amplitude_squared_CPU;
    double amplitude_squared_GPU;
    double phase_angle_CPU;
    double phase_angle_GPU;
    for (int m = 0; m < num_data_sets; m++){
        if (actual_num_elements == 16)
            addressGPU = m*total_output_frequencies*actual_num_elements*(actual_num_elements+1); // /2*2
        for (int freq = 0; freq < actual_num_frequencies; freq++){
            for (int element_y = 0; element_y < actual_num_elements; element_y++){
                for (int element_x = element_y; element_x < actual_num_elements; element_x++){
                    //compare real results
                    int data_Real_GPU = data_set_GPU[addressGPU++];
                    int data_Real_CPU = data_set_CPU[address++];
                    int difference_real = data_Real_GPU - data_Real_CPU;
                    //compare imaginary results
                    int data_Imag_GPU = data_set_GPU[addressGPU++];
                    int data_Imag_CPU = data_set_CPU[address++];
                    int difference_imag = data_Imag_GPU - data_Imag_CPU;

                    //get amplitude_squared
                    amplitude_squared_CPU = data_Real_CPU*data_Real_CPU + data_Imag_CPU*data_Imag_CPU;
                    amplitude_squared_GPU = data_Real_GPU*data_Real_GPU + data_Imag_GPU*data_Imag_GPU;
                    phase_angle_CPU = atan2((double)data_Imag_CPU,(double)data_Real_CPU);
                    phase_angle_GPU = atan2((double)data_Imag_GPU,(double)data_Real_GPU);

                    if (amplitude_squared_CPU != 0){
                        ratio_GPU_div_CPU[local_Address] = amplitude_squared_GPU/amplitude_squared_CPU;
                    }
                    else{
                        ratio_GPU_div_CPU[local_Address] = -1;//amplitude_squared_GPU/amplitude_squared_CPU;
                    }

                    phase_difference[local_Address++] = phase_angle_GPU - phase_angle_CPU;

                    if (difference_real != 0 || difference_imag !=0){
                        (*num_err)++;
                        if (verbosity){//if (verbosity && element_x==0 && element_y == 0&& freq ==0 ){
                            printf ("freq: %6d element_x: %6d element_y: %6d Real CPU/GPU %8d %8d Imaginary CPU/GPU %8d %8d ERR: %7d\n",freq, element_x, element_y, data_Real_CPU, data_Real_GPU, data_Imag_CPU, data_Imag_GPU, *num_err);
                        }
                        amplitude_squared_error = difference_imag*difference_imag+difference_real*difference_real;
                        *err_2 += amplitude_squared_error;
                        if (amplitude_squared_error > max_error)
                            max_error = amplitude_squared_error;
                    }
                    else{
                        if (verbosity){//if (verbosity&& element_x==0 && element_y == 0 && freq ==0 ){
                            printf ("freq: %6d element_x: %6d element_y: %6d Real CPU/GPU %8d %8d Imaginary CPU/GPU %8d %8d\n",freq, element_x, element_y, data_Real_CPU, data_Real_GPU, data_Imag_CPU, data_Imag_GPU);
                        }
                    }
                }
            }
        }
    }
    printf("\nTotal number of errors: %d, Sum of Squared Differences: %lld \n",*num_err, (long long int) *err_2);
    printf("sqrt(sum of squared differences/numberElements): %f \n", sqrt((*err_2)*1.0/local_Address));//add some more data--find maximum error, figure out other statistical properties
    printf("Maximum amplitude squared error: %d\n", max_error);
    return;
}

int main_generator_test (int argc, char ** argv){
    unsigned char *data_set = (unsigned char*) malloc(NUM_TIMESAMPLES);
    int counters[256];
    for (int i = 0; i < 256; i++)
        counters[i] = 0;

    //generate random set of numbers...
    generate_char_data_set(GEN_TYPE,
        GEN_DEFAULT_SEED, //random seed
        GEN_DEFAULT_RE,//default_real,
        GEN_DEFAULT_IM,//default_imaginary,
        GEN_INITIAL_RE,//initial_real,
        GEN_INITIAL_IM,//initial_imaginary,
        GEN_FREQ,//int single_frequency,
        NUM_TIMESAMPLES,//int num_timesteps,
        ACTUAL_NUM_FREQ,//int num_frequencies,
        ACTUAL_NUM_ELEM,//int num_elements,
        1,//NUM_DATA_SETS, need a 1 here because we have 1 long input array and many output arrays, NOT multiple input AND output arrays
        data_set);

    if (CHECKING_VERBOSE){
        print_element_data(17, NUM_FREQ, NUM_ELEM, ALL_FREQUENCIES, data_set);
    }

    for (int i = 0; i < NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ; i++){
        counters[HI_NIBBLE(data_set[i])]++;
        counters[LO_NIBBLE(data_set[i])]++;
        //printf
    }

    for (int i = 0; i < 256; i++){
        printf ("%d %d\n",i, counters[i]);
    }
    return 0;
}

int main(int argc, char ** argv) {
    double cputime=0;

    if (argc == 1){
        printf("This program expects the user to run the executable as \n $ ./executable GPU_card[0-3] num_repeats\n");
        return -1;
    }

    int dev_number = atoi(argv[1]);
    int nkern= atoi(argv[2]);//NUM_REPEATS_GPU;

    //basic setup of CL devices
    cl_int err;
    //cl_int err2;

    // 1. Get a platform.
    cl_platform_id platform;
    clGetPlatformIDs( 1, &platform, NULL );

    // 2. Find a gpu device.
    cl_device_id deviceID[5];

    err = clGetDeviceIDs( platform, CL_DEVICE_TYPE_GPU, 4, deviceID, NULL);

    if (err != CL_SUCCESS){
        printf("Error getting device IDs\n");
        return (-1);
    }
    cl_ulong lm;
    err = clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_LOCAL_MEM_SIZE, sizeof(cl_ulong), &lm, NULL);
    if (err != CL_SUCCESS){
        printf("Error getting device info\n");
        return (-1);
    }
    //printf("Local Mem: %i\n",lm);

    cl_uint mcl,mcm;
    clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(cl_uint), &mcl, NULL);
    clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &mcm, NULL);
    float card_tflops = mcl*1e6 * mcm*16*4*2 / 1e12;

    // 3. Create a context and command queues on that device.
    cl_context context = clCreateContext( NULL, 1, &deviceID[dev_number], NULL, NULL, NULL);
    cl_command_queue queue[N_QUEUES];
    for (int i = 0; i < N_QUEUES; i++){
        //queue[i] = clCreateCommandQueue( context, deviceID[dev_number], CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err );
        queue[i] = clCreateCommandQueue( context, deviceID[dev_number], CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE, &err );
        //add a more robust error check at some point?
        if (err){ //success returns a 0
            printf("Error initializing queues.  Exiting program.\n");
            return (-1);
        }

    }

    // 4. Perform runtime source compilation, and obtain kernel entry point.
    int size1_block = 32;
    int num_blocks = (NUM_ELEM / size1_block) * (NUM_ELEM / size1_block + 1) / 2.; // 256/32 = 8, so 8 * 9/2 (= 36) //needed for the define statement

    // 4a load the source files //this load routine is based off of example code in OpenCL in Action by Matthew Scarpino
    char cl_fileNames[4][256];
    sprintf(cl_fileNames[0],OPENCL_FILENAME_0);
    sprintf(cl_fileNames[1],OPENCL_FILENAME_1);
    if (XOR_MANUAL)
        sprintf(cl_fileNames[2],OPENCL_FILENAME_2b);
    else
        sprintf(cl_fileNames[2],OPENCL_FILENAME_2);
    sprintf(cl_fileNames[3],OPENCL_FILENAME_3);

    char cl_options[1024];
    sprintf(cl_options,"-g -D NUM_GATINGGROUPS=%d -D NUM_DATASETS=%d -D NUM_TIME_BINS=%d -D NUM_ZONES=%d -D ACTUAL_NUM_ELEMENTS=%du -D ACTUAL_NUM_FREQUENCIES=%du -D NUM_ELEMENTS=%du -D NUM_FREQUENCIES=%du -D NUM_BLOCKS=%du -D NUM_TIMESAMPLES=%du -D BASE_TIMESAMPLES_INT_ACCUM=%d", NUM_GATING_GROUPS, NUM_DATA_SETS, NUM_TIME_BINS, NUM_ZONES, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_ELEM, NUM_FREQ, num_blocks, NUM_TIMESAMPLES, BASE_TIMESAMPLES_ACCUM);// -O0 for optimization changes in compiler
    printf("Compiler Options: -g \n-D NUM_GATINGGROUPS=%d \n-D NUM_DATASETS=%d \n-D NUM_TIME_BINS=%d \n-D NUM_ZONES=%d \n-D ACTUAL_NUM_ELEMENTS=%du \n-D ACTUAL_NUM_FREQUENCIES=%du \n-D NUM_ELEMENTS=%du \n-D NUM_FREQUENCIES=%du \n-D NUM_BLOCKS=%du \n-D NUM_TIMESAMPLES=%du \n-D BASE_TIMESAMPLES_INT_ACCUM=%d \n", NUM_GATING_GROUPS, NUM_DATA_SETS, NUM_TIME_BINS, NUM_ZONES, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_ELEM, NUM_FREQ, num_blocks, NUM_TIMESAMPLES, BASE_TIMESAMPLES_ACCUM);// -O0 for optimization changes in compiler

    size_t cl_programSize[NUM_CL_FILES];
    FILE *fp;
    char *cl_programBuffer[NUM_CL_FILES];
    //printf("Hi\n");

    for (int i = 0; i < NUM_CL_FILES; i++){
        fp = fopen(cl_fileNames[i], "r");
        if (fp == NULL){
            printf("error loading file: %s\n", cl_fileNames[i]);
            return (-1);
        }
        fseek(fp, 0, SEEK_END);
        cl_programSize[i] = ftell(fp);
        rewind(fp);
        cl_programBuffer[i] = (char*)malloc(cl_programSize[i]+1);
        cl_programBuffer[i][cl_programSize[i]] = '\0';
        int sizeRead = fread(cl_programBuffer[i], sizeof(char), cl_programSize[i], fp);
        if (sizeRead < cl_programSize[i])
            printf("Error reading the file!!!");
        fclose(fp);
    }

    cl_program program = clCreateProgramWithSource( context, NUM_CL_FILES, (const char**)cl_programBuffer, cl_programSize, &err );
    if (err){
        printf("Error in clCreateProgramWithSource: %i\n",err);
        return(-1);
    }

    //printf("here1\n");
    err = clBuildProgram( program, 1, &deviceID[dev_number], cl_options, NULL, NULL );
    if (err){
        printf("Error in clBuildProgram: %i\n\n",err);
        size_t log_size;
        clGetProgramBuildInfo(program, deviceID[dev_number], CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char * program_log = (char*)malloc(log_size+1);
        program_log[log_size] = '\0';
        clGetProgramBuildInfo(program, deviceID[dev_number], CL_PROGRAM_BUILD_LOG, log_size+1, program_log, NULL);
        printf("%s\n",program_log);
        free(program_log);
        return(-1);
    }

//     cl_kernel kernel_time_shift_part_a = clCreateKernel( program, "time_shift_part_a", &err );
//     if (err){
//         printf("Error in clCreateKernel: %i\n\n",err);
//         return -1;
//     }
//     cl_kernel kernel_time_shift_part_b = clCreateKernel( program, "time_shift_part_b", &err );
//     if (err){
//         printf("Error in clCreateKernel: %i\n\n",err);
//         return -1;
//     }
//     /* Parameters are the same for parts a and b of the time shift kernels
//         __global unsigned char *data, //original array
//         __global unsigned char *intermediate_array, //a temporary array to store the intermediate data to avoid races and such
//                                                     //length = 2 x (#timesteps) x (#freq bins) x (#elements to shift)
//         int num_elements_to_shift,
//         int num_time_bins_to_shift, //can be positive or negative
//         int offset //0 or 1: tells if one is in the first or second half of the intermediate array
//     */

    cl_kernel time_shift_kernel = clCreateKernel(program, "time_shift", &err);
    if (err){
        printf("Error in clCreateKernel time_shift_kernel: %i\n\n",err);
        return -1;
    }
    /*
    __kernel void time_shift(__global unsigned char *data,
                            __global unsigned char *output_data,
                                    int            num_elements_to_shift,
                                    int            element_offset, //start # of elements to shift
                                    int            num_time_bins_to_shift,
                                    int            input_data_offset){
    */

    cl_kernel offsetAccumulate_kernel = clCreateKernel( program, "offsetAccumulateElements", &err );
    if (err){
        printf("Error in clCreateKernel offsetAccumulate_kernel: %i\n\n",err);
        return -1;
    }
    /* parameters for the kernel
    __kernel void offsetAccumulateElements (__global const uint *inputData,
                                            __global uint *outputData,
                                            __global uint *counters,
                                            __global const uint *offset_remainder, //a 'wrapped' offset time
                                            __global const uint *delta_time_bin, //time for 1 bin in the gating (in units of 10 ns) (size: GATING_GROUPS elements)
                                            __global const uint *bin_zone_mask)
     */

    cl_kernel preseed_kernel = clCreateKernel( program, "preseed", &err );
    if (err){
        printf("Error in clCreateKernel preseed_kernel: %i\n\n",err);
        return -1;
    }
    /*
    void preseed( __global const uint *dataIn,
              __global  int *corr_buf,
              __global const uint *id_x_map,
              __global const uint *id_y_map,
              __local  uint *localDataX, //think about removing this... this is slightly silly to have in the wrapper code
              __local  uint *localDataY,
              __global const uint *counters)
     */

    cl_kernel corr_kernel = clCreateKernel( program, "corr", &err );
    if (err){
        printf("Error in clCreateKernel corr_kernel: %i\n\n",err);
        return (-1);
    }
    /*
    void corr ( __global const uint *packed,            //input (packed complex data: each Byte has a 4+4 bit offset encoded complex number, RC)
            __global       int  *corr_buf,          //output
            __constant     uint *id_x_map,          //block-tile layout lookup table (x addresses of linear list of tiles) to be cached
            __constant     uint *id_y_map,          //block-tile layout lookup table (y)
            __global       int  *block_lock,        //locks for output table
            __global const uint *offset_remainder,  //
            __global const int  *delta_time_bin,    //array holding bin delta t values in 10 ns units. Size: GATING_GROUP_COUNT
            __global const int  *bin_zone_mask      //the lookup table showing which zone output values should go to (allows more than on/off gating groups)
          )
    */

    //printf("Made my cl kernels!\n");
    for (int i =0; i < NUM_CL_FILES; i++){
        free(cl_programBuffer[i]);
    }

    // 5. set up arrays and initilize if required
    cl_mem device_bin_counters          [N_STAGES];
    unsigned char *host_PrimaryInput    [N_STAGES]; //where things are brought from, ultimately. Code runs fastest when we create the aligned memory and then pin it to the device
    int *host_PrimaryOutput             [N_STAGES];
    cl_mem device_CLinput_pinnedBuffer  [N_STAGES];
    cl_mem device_CLoutput_pinnedBuffer [N_STAGES];
    // need to make input data contiguous, so will copy to different regions of a larger array
    //was this //cl_mem device_CLinput_kernelData    [N_STAGES];
    cl_mem device_CLinput_kernelData;
    cl_mem device_CLmodified_input_kernelData;//should only need one?
    cl_mem device_CLoutput_kernelData   [N_STAGES];
    cl_mem device_CLoutputAccum         [N_STAGES];
    cl_mem device_block_lock;

///TODO: check which things use len and which use NUM_DATA_SETS (double count of freq in len right now)
    int len=NUM_FREQ*num_blocks*(size1_block*size1_block)*2*NUM_DATA_SETS;// *2 real and imag //NUM_DATA_SETS now depends on NUM_GATING_GROUPS
    printf("Num_blocks %d ", num_blocks);
    printf("Output Length %d and size %ld B\n", len, len*sizeof(cl_int));
    int *zeros=calloc(num_blocks*NUM_FREQ,sizeof(int)); //for the output buffers ///TODO this agrees with the kernel but perhaps check if this is best

    device_block_lock = clCreateBuffer (context,
                                        CL_MEM_COPY_HOST_PTR,
                                        num_blocks*NUM_FREQ*sizeof(cl_int),
                                        zeros,
                                        &err);
    free(zeros);

    zeros=calloc(len,sizeof(cl_int)); //for the output buffers

    //posix_memalign ((void **)&host_CLinput_data, 4096, NUM_TIMESAMPLES*NUM_ELEM); //online it said that memalign was obsolete, so am using posix_memalign instead.  This should allow for pinning if desired.
    //they use getpagesize() for the demo's alignment size for Linux for Firepro, rather than the 4096 which was mentioned in the BufferBandwidth demo.
    //memalign taken care of by CL?

    printf("Size of Input data = %i B\n", NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
    printf("Number of Data Sets (for output) = %i\n", NUM_DATA_SETS);
    printf("Total Data size (input) = %d B\n", NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);

    // Set up arrays so that they can be used later on
    for (int i = 0; i < N_STAGES; i++){
        printf(".");
        //preallocate memory for pinned buffers
        err = posix_memalign ((void **)&host_PrimaryInput[i], PAGESIZE_MEM, NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
        //check if an extra command is needed to pre pin this--this might just make sure it is
        //aligned in memory space.
        if (err){
            printf("error in creating memory buffers: Inputa, stage: %i, err: %i. Exiting program.\n",i, err);
            return (err);
        }
        err = mlock(host_PrimaryInput[i], NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
        if (err){
            printf("error in creating memory buffers: Inputb, stage: %i, err: %i. Exiting program.\n",i, err);
            printf("%s",strerror(errno));
            return (err);
        }

        device_CLinput_pinnedBuffer[i] = clCreateBuffer ( context,
                                    CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,//
                                    NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ,
                                    host_PrimaryInput[i],
                                    &err); //create the clBuffer, using pre-pinned host memory

        if (err){
            printf("error in mapping pin pointers. Exiting program.\n");
            return (err);
        }

        err = posix_memalign ((void **)&host_PrimaryOutput[i], PAGESIZE_MEM, len*sizeof(cl_int));
        err |= mlock(host_PrimaryOutput[i],len*sizeof(cl_int));
        if (err){
            printf("error in creating memory buffers: Output, stage: %i. Exiting program.\n",i);
            return (err);
        }

        device_CLoutput_pinnedBuffer[i] = clCreateBuffer (context,
                                    CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
                                    len*sizeof(cl_int),
                                    host_PrimaryOutput[i],
                                    &err); //create the output buffer and allow cl to allocate host memory

        if (err){
            printf("error in mapping pin pointers. Exiting program.\n");
            return (err);
        }

        device_CLoutput_kernelData[i] = clCreateBuffer (context,
                                    CL_MEM_WRITE_ONLY|CL_MEM_COPY_HOST_PTR,
                                    len*sizeof(cl_int),
                                    zeros,
                                    &err); //cl memory that can only be written to by kernel--preset to 0s everywhere

        if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
        }

        device_bin_counters[i] = clCreateBuffer(context,
                                CL_MEM_READ_WRITE,
                                ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(cl_uint),
                                0,
                                &err);
        if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
        }

    } //end for
    //single array version for commented section above
    if (XOR_MANUAL){ //used to need read and write for this one... With new method that creates a new buffer for the device, possibly composed of time-shifted data, these arrays remain unchanged
        device_CLinput_kernelData = clCreateBuffer (context,
                                    CL_MEM_READ_ONLY,// | CL_MEM_USE_PERSISTENT_MEM_AMD, //ran out of memory when I tried to use this
                                    NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ*2,
                                    0,
                                    &err); //cl memory that can only be read by kernel

        if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
        }
    }
    else{
        device_CLinput_kernelData = clCreateBuffer (context,
                                    CL_MEM_READ_ONLY,// | CL_MEM_USE_PERSISTENT_MEM_AMD, //ran out of memory when I tried to use this
                                    NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ*2,
                                    0,
                                    &err); //cl memory that can only be read by kernel

        if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
        }
    }
    free(zeros);

    device_CLmodified_input_kernelData = clCreateBuffer(context,CL_MEM_READ_WRITE,NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ,0,&err);
    if (err){
        printf("error in allocating memory. Exiting program.\n");
        return (err);
    }

    //initialize an array for the accumulator of offsets (borrowed this buffer from an old version of code--check this)
///////
    zeros=calloc(NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS,sizeof(unsigned int)); // <--this was missed! Was causing most of the problems!
    unsigned int *zeros2 = calloc(ACTUAL_NUM_FREQ*NUM_DATA_SETS,sizeof(unsigned int));
//////
    device_CLoutputAccum[0] = clCreateBuffer(context,
                                          CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                          NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(unsigned int),
                                          zeros,
                                          &err);
    if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
    }

    device_CLoutputAccum[1] = clCreateBuffer(context,
                                          CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                                          NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(unsigned int),
                                          zeros,
                                          &err);
    if (err){
            printf("error in allocating memory. Exiting program.\n");
            return (err);
    }

    //arrays have been allocated

    //--------------------------------------------------------------
    //Generate Data Set!

    generate_char_data_set(GEN_TYPE,
                           GEN_DEFAULT_SEED, //random seed
                           GEN_DEFAULT_RE,//default_real,
                           GEN_DEFAULT_IM,//default_imaginary,
                           GEN_INITIAL_RE,//initial_real,
                           GEN_INITIAL_IM,//initial_imaginary,
                           GEN_FREQ,//int single_frequency,
                           NUM_TIMESAMPLES,//int num_timesteps,
                           ACTUAL_NUM_FREQ,//int num_frequencies,
                           ACTUAL_NUM_ELEM,//int num_elements,
                           1,//NUM_DATA_SETS, need a 1 here because we have 1 long input array and many output arrays, NOT multiple input AND output arrays
                           host_PrimaryInput[0]);

    if (NUM_ELEM <=32){
        if (INTERLEAVED){
            reorder_data_interleave_2_frequencies(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS,host_PrimaryInput[0]);
        }
    }
    //print_element_data(1, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, ALL_FREQUENCIES, host_PrimaryInput[0]);
    //reorder_data_phaseB_breakData(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, host_PrimaryInput[0]);

    memcpy(host_PrimaryInput[1], host_PrimaryInput[0], NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);

    //--------------------------------------------------------------


    // 6. Set up Kernel parameters

    //upper triangular address mapping --converting 1d addresses to 2d addresses
    unsigned int global_id_x_map[num_blocks];
    unsigned int global_id_y_map[num_blocks];

//     for (int i=0; i<num_blocks; i++){
//         int t = (int)(sqrt(1 + 8*(num_blocks-i-1))-1)/2; /*t is number of the current row, counting/increasing row numbers from the bottom, up, and starting at 0 --note it uses the property that converting to int uses a floor/truncates at the decimal*/
//         int y = NUM_ELEM/size1_block-t-1;
//         int x = (t+1)*(t+2)/2 + (i - num_blocks)+y;
//         global_id_x_map[i] = x;
//         global_id_y_map[i] = y;
//         printf("i = %d: t = %d, y = %d, x = %d \n", i, t, y, x);
//     }

    //TODO: p260 OpenCL in Action has a clever while loop that changes 1 D addresses to X & Y indices for an upper triangle.  Time Test kernels using
    //them compared to the lookup tables for NUM_ELEM = 256
    int largest_num_blocks_1D = NUM_ELEM/size1_block;
    int index_1D = 0;
    for (int j = 0; j < largest_num_blocks_1D; j++){
        for (int i = j; i < largest_num_blocks_1D; i++){
            global_id_x_map[index_1D] = i;
            global_id_y_map[index_1D] = j;
            index_1D++;
        }
    }

    cl_mem id_x_map = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    num_blocks * sizeof(cl_uint), global_id_x_map, &err);
    if (err){
        printf("Error in clCreateBuffer %i\n", err);
    }

    cl_mem id_y_map = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                    num_blocks * sizeof(cl_uint), global_id_y_map, &err);
    if (err){
        printf("Error in clCreateBuffer %i\n", err);
    }

    cl_uint num_gate_groups = NUM_GATING_GROUPS;
    cl_uint gate_size_time [NUM_GATING_GROUPS];
    //cl_uint num_time_bins [NUM_GATING_GROUPS];
    unsigned int *offset_remainder;
    offset_remainder = (unsigned int*)malloc(NUM_GATING_GROUPS*sizeof(unsigned int));
    //cl_uint clip_bin [NUM_GATING_GROUPS];

    unsigned int *zone_mask;
    zone_mask = (unsigned int *)malloc(ACTUAL_NUM_FREQ*NUM_TIME_BINS*NUM_GATING_GROUPS*sizeof(unsigned int));

    for (int i = 0 ; i < NUM_GATING_GROUPS; i++){
        gate_size_time[i] = GATE_PERIOD_IN_10ns_UNITS;
        //num_time_bins [i] = 128;
        //clip_bin [i]      = 0;
        offset_remainder[i] = 0; //default values...
        for (int j = 0; j < ACTUAL_NUM_FREQ; j++){

            for (int k = 0; k < NUM_TIME_BINS; k++){
                zone_mask[i*ACTUAL_NUM_FREQ*NUM_TIME_BINS+j*NUM_TIME_BINS+k] = ((i+j+k)/5)%NUM_ZONES;//need to fill the array with values in the range [0,NUM_ZONES-1]
            }
        }
    }

    cl_mem device_gate_size_time = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS*sizeof(cl_uint),gate_size_time, &err);
    if (err){
        printf("Error in setting gate periods. Error: %s\n",oclGetOpenCLErrorCodeStr(err));
        exit(err);
    }
    //cl_mem device_num_time_bins = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS *sizeof(cl_uint),num_time_bins,&err);
    //cl_mem device_clip_bin = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS * sizeof(cl_uint), clip_bin,&err);
    cl_mem device_offset_remainder = clCreateBuffer(context,CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS*sizeof(unsigned int), offset_remainder, &err);
    if (err){
        printf("Error in setting offset remainders. Error: %s\n",oclGetOpenCLErrorCodeStr(err));
        exit(err);
    }
    cl_mem device_zone_mask= clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, ACTUAL_NUM_FREQ*NUM_TIME_BINS*NUM_GATING_GROUPS*sizeof(unsigned int),zone_mask,&err);
    if (err){
        printf("Error in setting output zone masks. Error: %s\n",oclGetOpenCLErrorCodeStr(err));
        exit(err);
    }



    //set other parameters that will be fixed for the kernels (changeable parameters will be set in run loops)
// __kernel void time_shift(__global unsigned char *data,                                           0 // in general will change, but not now
//                          __global unsigned char *output_data,                                    1 // in general will change, but not now
//                                   int            num_elements_to_shift,                          2<--
//                                   int            element_offset, //start # of elements to shift  3<--
//                                   int            num_time_bins_to_shift,                         4<--
//                                   int            input_data_offset){                             5<--
    clSetKernelArg(time_shift_kernel, 0, sizeof(void *), (void *)&device_CLinput_kernelData);//the original address is just offset with param 5--this stays the same.
    clSetKernelArg(time_shift_kernel, 1, sizeof(void *), (void *)&device_CLmodified_input_kernelData);//data always goes to the same spot

    /* parameters for the kernel
    __kernel void offsetAccumulateElements (__global const uint *inputData, //0 <--
                                        __global uint *outputData,          //1 <--
                                        __global uint *counters,            //2 <--
                                        __global const uint *offset_remainder, //a 'wrapped' offset time
                                        __global const uint *delta_time_bin, //time for 1 bin in the gating (in units of 10 ns) (buffer size: GATING_GROUPS elements)
                                        __global const uint *bin_zone_mask)
         */
    clSetKernelArg(offsetAccumulate_kernel, 3, sizeof(void *), (void *)&device_offset_remainder);
    clSetKernelArg(offsetAccumulate_kernel, 4, sizeof(void *), (void *)&device_gate_size_time);
    clSetKernelArg(offsetAccumulate_kernel, 5, sizeof(void *), (void *)&device_zone_mask);



    /*
    void preseed( __global const uint *dataIn,  //0 <--
              __global  int *corr_buf,          //1 <--
              __global const uint *id_x_map,    //2
              __global const uint *id_y_map,    //3
              __global const uint *counters)    //4 <--
     */
    clSetKernelArg(preseed_kernel, 2, sizeof(id_x_map), (void*) &id_x_map);
    clSetKernelArg(preseed_kernel, 3, sizeof(id_y_map), (void*) &id_y_map);


    //need to keep in mind a number of things and make sure that new additions do not break old parts...
     /*
    void corr ( __global const uint *packed,            //  //0 input (packed complex data: each Byte has a 4+4 bit offset encoded complex number, RC)
                __global       int  *corr_buf,          //  //1 output
                __constant     uint *id_x_map,          //2 block-tile layout lookup table (x addresses of linear list of tiles) to be cached
                __constant     uint *id_y_map,          //3 block-tile layout lookup table (y)
                __global       int  *block_lock,        //4 locks for output table
                __global const uint *offset_remainder,  //  //5 array holding the time offsets related to the gate_period to sync to the zones in out_bin_number
                __global const uint *delta_time_bin,    //6 array holding bin delta t values in 10 ns units. Size: GATING_GROUP_COUNT
                __global const uint *bin_zone_mask      //7 the lookup table showing which zone output values should go to (allows more than on/off gating groups)
    */
    clSetKernelArg(corr_kernel, 2, sizeof(void *), (void*) &id_x_map); //this should maybe be sizeof(void *)?
    clSetKernelArg(corr_kernel, 3, sizeof(void *), (void*) &id_y_map);
    clSetKernelArg(corr_kernel, 4, sizeof(void *), (void*) &device_block_lock);
    clSetKernelArg(corr_kernel, 6, sizeof(void *), (void *)&device_gate_size_time);
    clSetKernelArg(corr_kernel, 7, sizeof(void *), (void *)&device_zone_mask);


    size_t gws_time_shift[3] = {ACTUAL_NUM_ELEM,ACTUAL_NUM_FREQ,NUM_TIMESAMPLES};
    int elem_mod;
    int freq_mod;
    if (ACTUAL_NUM_ELEM>64){
        elem_mod = 64;
        freq_mod = 1;
    }
    else{
        elem_mod = ACTUAL_NUM_ELEM;
        freq_mod = 64/ACTUAL_NUM_ELEM;
    }
    size_t lws_time_shift[3] = {elem_mod,freq_mod,1};
    printf("GWS: %d %d %d\nLWS: %d %d %d\n", (int)gws_time_shift[0],(int)gws_time_shift[1],(int)gws_time_shift[2],(int)lws_time_shift[0],(int)lws_time_shift[1],(int)lws_time_shift[2]);

    unsigned int n_cAccum=NUM_TIMESAMPLES/256u; //n_cAccum == number_of_compressedAccum
    size_t gws_accum[3]={64, (int)ceil(NUM_ELEM*NUM_FREQ/256.0),NUM_TIMESAMPLES/BASE_TIMESAMPLES_ACCUM};
    size_t lws_accum[3]={64, 1, 1};

    printf("(int)ceil(NUM_ELEM*NUM_FREQ/256.0): %d\n", (int)ceil(NUM_ELEM*NUM_FREQ/256.0));
    size_t gws_preseed[3]={8*NUM_DATA_SETS, 8*NUM_FREQ, num_blocks};
    size_t lws_preseed[3]={8, 8, 1};

    size_t gws_corr[3]={8,8*NUM_FREQ,num_blocks*n_cAccum}; //global work size array
    size_t lws_corr[3]={8,8,1}; //local work size array
    //int *corr_ptr;

    //setup and start loop to process data in parallel
    int spinCount = 0; //we rotate through values to launch processes in order for the command queues. This helps keep track of what position, and can run indefinitely without overflow
    int writeToDevStageIndex;
    int kernelStageIndex;
    //int readFromDevStageIndex;

    cl_int numWaitEventWrite = 0;
    cl_event* eventWaitPtr = NULL;
    cl_event clearCountersEvent;
    cl_event time_shift_event;
    cl_event copy_time_remainders_event;

    cl_event lastWriteEvent[N_STAGES]  = { 0 }; // All entries initialized to 0, since unspecified entries are set to 0
    cl_event lastKernelEvent[N_STAGES] = { 0 };
    //cl_event lastReadEvent[N_STAGES]   = { 0 };
    cl_event copyInputDataEvent;
    cl_event offsetAccumulateEvent;
    cl_event preseedEvent;

    if (TIMER_FOR_PROCESSING_ONLY){
        for (int i = 0; i < N_STAGES; i++){
             err = clEnqueueWriteBuffer(queue[0],
                                    device_CLinput_kernelData, //to here
                                    CL_TRUE,
                                    i*NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ, //offset (then puts things in a larger contiguous block of memory)
                                    NUM_TIMESAMPLES * NUM_ELEM*NUM_FREQ, //
                                    host_PrimaryInput[i], //from here
                                    0,
                                    NULL,
                                    NULL);
            if (err){
                printf("Error in transfer to device memory. Error in loop %d, error: %s\n",i,oclGetOpenCLErrorCodeStr(err));
                exit(err);
            }
        }
        clFinish(queue[0]);
        printf("copy complete\n");
    }

    ///////////////////////////////////////////////////////////////////////////////
    cl_int num_elem_to_shift = 2;
    cl_int element_offset = 1;
    cl_int time_bins_to_shift = -2;//shift the first two elements back 2 time step; that is, assign t2 to t0, t3 to t1, etc
    cl_int input_data_offset;
    cl_int extra_offset;
    if (time_bins_to_shift < 0)
        extra_offset = 0;
    else
        extra_offset = 1;

    cputime = e_time();

    //if (nkern >1){
        for (int i=0; i<=nkern; i++){//if we were truly streaming data, for each correlation, we would need to change what arrays are used for input/output
            //printf("spinCount %d, i: %d %d\n",spinCount, i&0x1, i);
            writeToDevStageIndex =  (spinCount ); // + 0) % N_STAGES;
            kernelStageIndex =      (spinCount + 1 + extra_offset) % N_STAGES; //had been + 2 when it was 3 stages
            input_data_offset = kernelStageIndex;
            //readFromDevStageIndex = (spinCount + 1 ) % N_STAGES;

            uint wrapped_time_offset = 0; //should be the current_time%(time_per_bin*NUM_DATA_SETS)
            for (int g_group = 0; g_group <= num_gate_groups; g_group++){
                offset_remainder[g_group] = wrapped_time_offset;
            }
            //transfer section
            if (i <= nkern){ //Start at 0,
                //check if it needs to wait on anything
                if(lastKernelEvent[writeToDevStageIndex] != 0){ //only equals 0 when it hasn't yet been defined i.e. the first run through the loop with N_STAGES == 2
                    numWaitEventWrite = 1;
                    eventWaitPtr = &lastKernelEvent[writeToDevStageIndex]; //writes must wait on the last kernel operation since
                }
                else {
                    numWaitEventWrite = 0;
                    eventWaitPtr = NULL;
                    }

                //copy necessary buffers to device memory
                if (TIMER_FOR_PROCESSING_ONLY){
                    //clearCountersEvent
                    //copy_time_remainders_event
                    //initialize the the accumulation buffer later used for correcting the output
                    err = clEnqueueWriteBuffer(queue[0],
                                            device_bin_counters[writeToDevStageIndex],
                                            CL_FALSE,
                                            0,
                                            ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(cl_uint),
                                            zeros2, //from here
                                            numWaitEventWrite,
                                            eventWaitPtr,
                                            &clearCountersEvent);



                    if (eventWaitPtr != NULL)
                        clReleaseEvent(*eventWaitPtr);
                    err = clEnqueueWriteBuffer(queue[0],
                                            device_offset_remainder,
                                            CL_FALSE,
                                            0,
                                            NUM_GATING_GROUPS*sizeof(cl_uint),
                                            offset_remainder, //from here
                                            1,
                                            &clearCountersEvent,
                                            &copy_time_remainders_event);

                    clReleaseEvent(clearCountersEvent);


                     err = clEnqueueWriteBuffer(queue[0],
                                            device_CLoutputAccum[writeToDevStageIndex],//to here (device memory)
                                            CL_FALSE,
                                            0,
                                            NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(cl_int),
                                            zeros, //from here (host memory)
                                            1,
                                            &copy_time_remainders_event,
                                            &lastWriteEvent[writeToDevStageIndex]);

                    clReleaseEvent(copy_time_remainders_event);
                    //printf("writeEvent: %d\n", writeToDevStageIndex);
                    err = clFlush(queue[0]);
                    if (err){
                        printf("Error in flushing transfer to device memory. Error in loop %d\n",i);
                        exit(err);
                    }

                }
                else{ //not speed testing--regular operation
                    err = clEnqueueWriteBuffer(queue[0],
                                            device_CLinput_kernelData, //to here
                                            CL_FALSE,
                                            (writeToDevStageIndex)*NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ, //offset (writeToDevStageIndex is always 0 or 1)
                                            NUM_TIMESAMPLES * NUM_ELEM*NUM_FREQ, //
                                            host_PrimaryInput[writeToDevStageIndex], //from here
                                            numWaitEventWrite,
                                            eventWaitPtr,
                                            &copyInputDataEvent);
                    //printf(".");
                    if (eventWaitPtr != NULL)
                        clReleaseEvent(*eventWaitPtr);

                    err = clEnqueueWriteBuffer(queue[0],
                                            device_bin_counters[writeToDevStageIndex],
                                            CL_FALSE,
                                            0,
                                            ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(unsigned int),
                                            zeros2,
                                            1,
                                            &copyInputDataEvent,
                                            &clearCountersEvent);
                    if (err){
                        printf("Error in transfer to device memory. Error in loop %d, error: %s\n",i,oclGetOpenCLErrorCodeStr(err));
                        exit(err);
                    }

                    clReleaseEvent(copyInputDataEvent);
                    err = clEnqueueWriteBuffer(queue[0],
                                            device_CLoutputAccum[writeToDevStageIndex],
                                            CL_FALSE,
                                            0,
                                            NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(int),
                                            zeros,
                                            1,
                                            &clearCountersEvent,
                                            &lastWriteEvent[writeToDevStageIndex]);

                    clReleaseEvent(clearCountersEvent);
                    //printf("writeEvent: %d\n", writeToDevStageIndex);
//                     err = clFlush(queue[0]);
//                     if (err){
//                         printf("Error in flushing transfer to device memory. Error in loop %d\n",i);
//                         exit(err);
//                     }
                }
            }
            //printf("hello");

            //processing section
            if (i <= nkern && i > 0){//insert additional steps for processing here--this condition ensures that 2 buffers have been loaded
                //timeshift to new array
                //required steps include: offset accumulator (order(Num_elements*Num_frequencies*Num_timesteps))
                //then pre-seed output array
                //then perform the correlation

                //set time_shift_kernel arguments
    //             __kernel void time_shift(__global unsigned char *data,           //0
    //                                      __global unsigned char *output_data,    //1
    //                                      int            num_elements_to_shift,   //2
    //                                      int            element_offset,          //3
    //                                      int            num_time_bins_to_shift,  //4
    //                                      int            input_data_offset){      //5
    //
                //printf("Before timeshift\n");
                err = clSetKernelArg(time_shift_kernel,
                                    2,
                                    sizeof(cl_int),
                                    &num_elem_to_shift);
                err |= clSetKernelArg(time_shift_kernel,
                                    3,
                                    sizeof(cl_int),
                                    &element_offset);
                err |= clSetKernelArg(time_shift_kernel,
                                    4,
                                    sizeof(cl_int),
                                    &time_bins_to_shift);
                err |= clSetKernelArg(time_shift_kernel,
                                    5,
                                    sizeof(cl_int),
                                    &input_data_offset);
                if (err){
                    printf("Error setting the time shift kernel arguments in loop %d\n", i);
                    exit(err);
                }
                err = clEnqueueNDRangeKernel(queue[1],
                                            time_shift_kernel,
                                            3,
                                            NULL,
                                            gws_time_shift,
                                            lws_time_shift,
                                            1,
                                            &lastWriteEvent[i&0x1], // make sure data is present, first
                                            &time_shift_event);
                if (err){
                    printf("Error time shifting in loop %d: Err: %d %s\n", i, err, oclGetOpenCLErrorCodeStr(err));
                    exit(err);
                }
                //printf("After timeshift\n");
                clReleaseEvent(lastWriteEvent[i&0x1]);

//accumulateFeeds_kernel--set 3 arguments--input array, zeroed output array, and counters
                err = clSetKernelArg(offsetAccumulate_kernel,
                                    0,
                                    sizeof(void*),
                                    (void*) &device_CLmodified_input_kernelData);

                err |= clSetKernelArg(offsetAccumulate_kernel,
                                    1,
                                    sizeof(void *),
                                    (void *) &device_CLoutputAccum[kernelStageIndex]); //make sure this array is zeroed initially!
                err |= clSetKernelArg(offsetAccumulate_kernel,
                                    2,
                                    sizeof(void *),
                                    (void *) &device_bin_counters[kernelStageIndex]);
                //err |= clSetKernelArg(offsetAccumulate_kernel,
                //                    5,
                //                    sizeof(void *),
                //                    &device_offset_remainder);
                if (err){
                    printf("Error setting the 0th kernel arguments in loop %d\n", i);
                    exit(err);
                }
//                 else{
//                     printf("ok\n");
//                 }

                err = clEnqueueNDRangeKernel(queue[1],
                                            offsetAccumulate_kernel,
                                            3,
                                            NULL,
                                            gws_accum,
                                            lws_accum,
                                            1,
                                            &time_shift_event, // make sure data is present, first
                                            &offsetAccumulateEvent);
                if (err){
                    printf("Error accumulating in loop %d\n", i);
                    exit(err);
                }

                clReleaseEvent(time_shift_event);
// preseed_kernel--set only 3 of the 7 arguments (the other 4 stay the same)
//                 void preseed( __global const uint *dataIn,   0
//               __global  int *corr_buf,                       1
//               __global const uint *id_x_map,                 2
//               __global const uint *id_y_map,                 3
//               __global const uint *counters)                 4
                err = clSetKernelArg(preseed_kernel,
                                    0,
                                    sizeof(void *),
                                    (void *) &device_CLoutputAccum[kernelStageIndex]);//assign the accumulated data as input

                if (err){
                    printf("Error setting param 0 in the 1st kernel arguments in loop %d\n", i);
                    exit(err);
                }
                err = clSetKernelArg(preseed_kernel,
                                    1,
                                    sizeof(void *),
                                    (void *) &device_CLoutput_kernelData[kernelStageIndex]); //set the output for preseeding the correlator array

                if (err){
                    printf("Error setting param 1 in the 1st kernel arguments in loop %d\n", i);
                    exit(err);
                }
                assert(kernelStageIndex<N_STAGES);
                assert(device_bin_counters!= NULL);
                printf("kernelStageIndex: %d\n",kernelStageIndex);
                err = clSetKernelArg(preseed_kernel,
                                    4,
                                    sizeof(void *),
                                    (void *) &device_bin_counters[kernelStageIndex]); //set the output for preseeding the correlator array

                if (err){
                    printf("Error setting param 4 in the 1st kernel arguments in loop %d\n", i);
                    exit(err);
                }

                err = clEnqueueNDRangeKernel(queue[1],
                                            preseed_kernel,
                                            3, //3d global dimension, also worksize
                                            NULL, //no offsets
                                            gws_preseed,
                                            lws_preseed,
                                            1,
                                            &offsetAccumulateEvent,//dependent on previous step so don't use &lastWriteEvent[kernelStageIndex],
                                            &preseedEvent);
                if (err){
                    printf("Error performing preseed kernel operation in loop %d: error %d\n", i,err);
                    exit(err);
                }

                clReleaseEvent(offsetAccumulateEvent);
                //corr_kernel--set the input and output buffers (the other parameters stay the same).
                err =  clSetKernelArg(corr_kernel,
                                        0,
                                        sizeof(void *),
                                        (void*) &device_CLmodified_input_kernelData);

                err |= clSetKernelArg(corr_kernel,
                                        1,
                                        sizeof(void *),//sizeof(void *)
                                        (void*) &device_CLoutput_kernelData[kernelStageIndex]);


                err |= clSetKernelArg(corr_kernel,
                                        5,
                                        sizeof(void *),
                                        &device_offset_remainder);


                if (err){
                    printf("Error setting the 2nd kernel arguments in loop %d\n", i);
                    exit(err);
                }

                err = clEnqueueNDRangeKernel(queue[1],
                                            corr_kernel,
                                            3, //3d global dimension, also worksize
                                            NULL, //no offsets
                                            gws_corr,
                                            lws_corr,
                                            1,
                                            &preseedEvent,//&lastWriteEvent[kernelStageIndex],//&preseedEvent,//dependent on previous step so don't use &lastWriteEvent[kernelStageIndex],
                                            &lastKernelEvent[kernelStageIndex]);
                if (err){
                    printf("Error performing corr kernel operation in loop %d, err: %d\n", i,err);
                    exit(err);
                }
                clReleaseEvent(preseedEvent);
                //printf("kernelStageIndex %i\n", kernelStageIndex);
                //if (kernelStageIndex == 1)
                //  err = clFinish(queue[1]);
                //else
//                     err = clFlush(queue[1]);
//                 if (err){
//                     printf("Error in flushing kernel run. Error in loop %d\n",i);
//                     exit(err);
//                 }

            }

            //since the kernel accumulates results, it isn't necessary/wanted to have it pull out results each time
            //processing of the results would need to be done, arrays reset (or kernel changed); the transfers could also slow things down, too....
//             if (i%10==0){
//                 err =  clFinish(queue[0]);
//                 err |= clFinish(queue[1]);
//
//                 if (err){
//                     printf("Error while finishing up the queue after the loops.\n");
//                     return (err);
//                 }
//             }
            spinCount++;
            spinCount = (spinCount < N_STAGES) ? spinCount : 0; //keeps the value of spinCount small, always, and then saves 1 remainder calculation earlier in the loop.
        }
    //}

    //since there are only 2, simplify things (i.e. no need for a loop).
    err =  clFinish(queue[0]);
    err |= clFinish(queue[1]);

    if (err){
        printf("Error while finishing up the queue after the loops.\n");
        return (err);
    }
 //printf("HELLO\n");
    //if (nkern >1){
//         if (preseedEvent != NULL)
//             clReleaseEvent(preseedEvent);
//         if (copyInputDataEvent != NULL)
//             clReleaseEvent(copyInputDataEvent);
//         if (offsetAccumulateEvent != NULL)
//             clReleaseEvent(offsetAccumulateEvent);
//         if (lastKernelEvent[0] != NULL)
//             clReleaseEvent(lastKernelEvent[0]);
//         if (lastKernelEvent[1] != NULL)
//             clReleaseEvent(lastKernelEvent[1]);
//         if (lastWriteEvent[0] != NULL)
//             clReleaseEvent(lastWriteEvent[0]);
//         if (lastWriteEvent[1] != NULL)
//             clReleaseEvent(lastWriteEvent[1]);
    //}
    // 7. Look at the results via synchronous buffer map.
    //int *corr_ptr;
    //printf(".");
    if (CHECKING_VERBOSE){
        print_element_data(17, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, 2, host_PrimaryInput[0]);
    }
    err = clEnqueueReadBuffer(queue[0], device_CLmodified_input_kernelData, CL_TRUE, 0, NUM_TIMESAMPLES*NUM_FREQ*NUM_ELEM,host_PrimaryInput[0],0,NULL,NULL);
    if (err){
        printf("Error reading data back to host.\n");
        //return (err);
    }
    err = clFinish(queue[0]);
    if (CHECKING_VERBOSE){
        print_element_data(17, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, 2, host_PrimaryInput[0]);
    }
    printf("\n\n");
    err = clEnqueueReadBuffer(queue[0], device_CLoutput_kernelData[0], CL_TRUE, 0, len*sizeof(cl_int), host_PrimaryOutput[0], 0, NULL, NULL);
    err |= clEnqueueReadBuffer(queue[0], device_CLoutput_kernelData[1], CL_TRUE, 0, len*sizeof(cl_int), host_PrimaryOutput[1], 0, NULL, NULL);

    if (err){
        printf("Error reading data back to host.\n");
        //return (err);
    }

    err = clFinish(queue[0]);

    if (err){
        printf("Error while finishing up the queue after the loops.\n");
        //return (err);
    }

    //printf("to transfer part 1\n");
    //accumulate results into one array
    //unmap output?

    printf("Running %i iterations of full corr (%i time samples (%i Ki time samples), %i elements, %i frequencies, %i data set", nkern, NUM_TIMESAMPLES, NUM_TIMESAMPLES/1024, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_DATA_SETS);

    if (NUM_DATA_SETS == 1)
        printf(")\n");
    else
        printf("s)\n");

    cputime = e_time()-cputime;
    if (nkern > 1){
        for (int i = 0; i < len; i++){
            //dump out results
            if (DEBUG){
                printf("%d ",host_PrimaryOutput[0][i]);
                if ((i+1) % (32 * 32* 2) == 0)
                    printf("\n");
            }
            //host_CLoutput_data[0][i] += host_CLoutput_data[1][i];
            host_PrimaryOutput[0][i] += host_PrimaryOutput[1][i];
            host_PrimaryOutput[0][i] /=2; //the results in output 0 and 1 should be identical--this is just to check (in a rough way) that they are.
            //if the average of the two arrays is the correct answer, and one expects both of them to have an answer, then the answers of both should be correct
        }
    }
    //printf("to transfer part 2\n");
    //--------------------------------------------------------------
    //--------------------------------------------------------------
    double unpack_Rate =(1.0*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern) /cputime/1000.0 ;// /cputime/1000.0;
    printf("Unpacking rate: %6.4fs on GPU (%.1f kHz)\n",cputime,unpack_Rate);
    printf("    [Theoretical max: @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
                                    card_tflops*1e12 / (ACTUAL_NUM_ELEM/2.*(ACTUAL_NUM_ELEM+1.) * 2. * 2.) / 1e3,
                                    100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (ACTUAL_NUM_ELEM/2.*(ACTUAL_NUM_ELEM+1.) * 2. * 2.)));
    if (ACTUAL_NUM_ELEM == 16){
        printf("    [Algorithm max:   @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
                                    card_tflops*1e12 / (num_blocks * 16 * 16 * 2. * 2.) / 1e3,
                                    100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (num_blocks * 16 * 16 * 2. * 2.)));
    }
    else{
        printf("    [Algorithm max:   @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
                                    card_tflops*1e12 / (num_blocks * size1_block * size1_block * 2. * 2.) / 1e3,
                                    100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (num_blocks * size1_block * size1_block * 2. * 2.)));

    }
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//continue search for NUM_DATA_SETS after this point--could cause bugs if misused
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    int *correlated_CPU;
    int *correlated_GPU;
    double *amp2_ratio_GPU_div_CPU;
    double *phaseAngleDiff_GPU_m_CPU;
    if (VERIFY_RESULTS){
        // start using calls to do the comparisons
        cputime = e_time();
        correlated_CPU = calloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM))*ACTUAL_NUM_FREQ*2*NUM_DATA_SETS,sizeof(int)); //made for the largest possible size (one size fits all)
        //int *correlated_CPU = calloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM+1))/2*ACTUAL_NUM_FREQ*2,sizeof(int));
        if (correlated_CPU == NULL){
            printf("failed to allocate memory\n");
            return(-1);
        }

        //cl_int num_elem_to_shift = 3;
        //cl_int element_offset = 1;
        //cl_int time_bins_to_shift = -5;/
    //    err = cpu_data_generate_and_correlate(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, correlated_CPU);
        //int cpu_data_generate_and_correlate_upper_triangle_only_gated_general(int num_timesteps, int num_frequencies, int num_elements, unsigned int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data_triangle, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period){
        //int cpu_data_generate_and_correlate_gated_general(int num_timesteps, int num_frequencies, int num_elements, int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data, int num_gate_groups, int *time_offset_remainder, int *single_gate_period){
        if (UPPER_TRIANGLE){
            err = cpu_data_generate_and_correlate_upper_triangle_only_gated_general_time_shifting(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, zone_mask, correlated_CPU,NUM_GATING_GROUPS,offset_remainder,gate_size_time,num_elem_to_shift,element_offset,time_bins_to_shift,1);
        }
        else{
            err = cpu_data_generate_and_correlate_gated_general_time_shifting(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, zone_mask, correlated_CPU, NUM_GATING_GROUPS, offset_remainder,gate_size_time,num_elem_to_shift,element_offset,time_bins_to_shift,1);
        }

        correlated_GPU = (int *)malloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM))*HDF5_FREQ*2*NUM_DATA_SETS*sizeof(int));
        //int *correlated_GPU = (int *)malloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM+1))/2*ACTUAL_NUM_FREQ*2*sizeof(int));
        if (correlated_GPU == NULL){
            printf("failed to allocate memory\n");
            return(-1);
        }

        if (ACTUAL_NUM_ELEM == 16){
            //printf("Hey!\n");
            if (INTERLEAVED){
                //printf("I was interleaved!\n");
                reorganize_32_to_16_feed_GPU_Correlated_Data_Interleaved(ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0]);
            }
            else{
                reorganize_32_to_16_feed_GPU_Correlated_Data(ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0]); //needed for comparison of outputs.
            }

            if (UPPER_TRIANGLE){
                reorganize_data_16_element_with_triangle_conversion(HDF5_FREQ, ACTUAL_NUM_FREQ,NUM_DATA_SETS,host_PrimaryOutput[0],correlated_GPU);
            }
        }
        else{
            if (UPPER_TRIANGLE){
                reorganize_GPU_to_upper_triangle(size1_block, num_blocks, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0], correlated_GPU);
            }
            else{
                reorganize_GPU_to_full_Matrix_for_comparison(size1_block, num_blocks, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0], correlated_GPU);
            }
        }
        //correct_GPU_correlation_results (NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, host_PrimaryOutput[0], host_outputAccum); //host side correction not needed now.
        int number_errors = 0;
        int64_t errors_squared;
        amp2_ratio_GPU_div_CPU = (double *)malloc(ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(double));
        if (amp2_ratio_GPU_div_CPU == NULL){
            printf("ran out of memory\n");
            return (-1);
        }
        phaseAngleDiff_GPU_m_CPU = (double *)malloc(ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(double));
        if (phaseAngleDiff_GPU_m_CPU == NULL){
            printf("2ran out of memory\n");
            return (-1);
        }

        if (UPPER_TRIANGLE){
            compare_NSquared_correlator_results_data_has_upper_triangle_only ( &number_errors, &errors_squared, HDF5_FREQ, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, correlated_GPU, correlated_CPU, amp2_ratio_GPU_div_CPU, phaseAngleDiff_GPU_m_CPU, CHECKING_VERBOSE);
        }
        else{
            compare_NSquared_correlator_results ( &number_errors, &errors_squared, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, correlated_GPU, correlated_CPU, amp2_ratio_GPU_div_CPU, phaseAngleDiff_GPU_m_CPU, CHECKING_VERBOSE);
        }

        if (number_errors > 0)
            printf("Error with correlation/accumulation! Num Err: %d and length of correlated data: %d\n",number_errors, ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ);
        else
            printf("Correlation/accumulation successful! CPU matches GPU.\n");
        //printf ("idx = %d\n", idx);
        cputime=e_time()-cputime;
        printf("Full Corr: %4.2fs on CPU (%.2f kHz)\n",cputime,NUM_TIMESAMPLES/cputime/1e3);
    }
    else{
        printf("Results unverified.  Use this solely as a gpu processing rate indicator, and verify results before adopting all settings\n");
    }
    err = munlockall();


    for (int ns=0; ns < N_STAGES; ns++){
        err = clReleaseMemObject(device_CLinput_pinnedBuffer[ns]);
        if (err != SDK_SUCCESS) {
            printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
            printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
            exit(err);
        }
        err = clReleaseMemObject( device_CLoutput_pinnedBuffer[ns]);
        if (err != SDK_SUCCESS) {
            printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
            printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
            exit(err);
        }

        err = clReleaseMemObject(device_CLoutput_kernelData[ns]);
        if (err != SDK_SUCCESS) {
            printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
            printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
            exit(err);
        }
        err = clReleaseMemObject(device_bin_counters[ns]);
        if (err != SDK_SUCCESS) {
            printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
            printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
            exit(err);
        }
        assert(host_PrimaryInput[ns]!=NULL);
        free(host_PrimaryInput[ns]);
        free(host_PrimaryOutput[ns]);

        //err = clReleaseMemObject(device_CLoutputAccum_pinnedBuffer[ns]);
        //if (err != SDK_SUCCESS) {
        //    printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
        //    printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
        //    exit(err);
        //}
        err = clReleaseMemObject(device_CLoutputAccum[ns]);
        if (err != SDK_SUCCESS) {
            printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
            printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
            exit(err);
        }
    }

    err = clReleaseMemObject(device_CLinput_kernelData);
    if (err != SDK_SUCCESS) {
        printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
        printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
        exit(err);
    }

    err = clReleaseMemObject(device_gate_size_time);
    if (err != SDK_SUCCESS) {
        printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
        printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
        exit(err);
    }

    err = clReleaseMemObject(device_offset_remainder);
    if (err != SDK_SUCCESS) {
        printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
        printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
        exit(err);
    }

    err = clReleaseMemObject(device_zone_mask);
    if (err != SDK_SUCCESS) {
        printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
        printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
        exit(err);
    }
    //free(corr_re);
    //free(corr_im);
    if (VERIFY_RESULTS){
        free(correlated_CPU);
        free(correlated_GPU);

        //free(host_outputAccum);
        free(amp2_ratio_GPU_div_CPU);
        free(phaseAngleDiff_GPU_m_CPU);
    }
    //free(accum_re);
    //free(accum_im);

    free(zeros);
    free(zeros2);
    free(zone_mask);
    free(offset_remainder);
    //--------------------------------------------------------------
    //--------------------------------------------------------------


    //free(data_block);
    //free(input_data_2);
    //free(output_data_1);
    //free(output_data_2);

    //err = clEnqueueUnmapMemObject(queue,corr_buffer,corr_ptr,0,NULL,NULL);
    //if (err) printf("Error in clEnqueueUnmapMemObject!\n");
    //clFinish(queue);

    clReleaseMemObject(device_block_lock);
    clReleaseKernel(corr_kernel);
    clReleaseProgram(program);
    //clReleaseMemObject(input_buffer);
    //clReleaseMemObject(input_buffer2);
    //clReleaseMemObject(copy_buffer);
    //clReleaseMemObject(copy_buffer2);
    //clReleaseMemObject(corr_buffer);
    clReleaseMemObject(id_x_map);
    clReleaseMemObject(id_y_map);
    clReleaseCommandQueue(queue[0]);
    clReleaseCommandQueue(queue[1]);
    clReleaseContext(context);
    return 0;
}

// int main_old(int argc, char ** argv) {
//     double cputime=0;
//
//     if (argc == 1){
//         printf("This program expects the user to run the executable as \n $ ./executable GPU_card[0-3] num_repeats\n");
//         return -1;
//     }
//
//     int dev_number = atoi(argv[1]);
//     int nkern= atoi(argv[2]);//NUM_REPEATS_GPU;
//
//     //basic setup of CL devices
//     cl_int err;
//     //cl_int err2;
//
//     // 1. Get a platform.
//     cl_platform_id platform;
//     clGetPlatformIDs( 1, &platform, NULL );
//
//     // 2. Find a gpu device.
//     cl_device_id deviceID[5];
//
//     err = clGetDeviceIDs( platform, CL_DEVICE_TYPE_GPU, 4, deviceID, NULL);
//
//     if (err != CL_SUCCESS){
//         printf("Error getting device IDs\n");
//         return (-1);
//     }
//     cl_ulong lm;
//     err = clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_LOCAL_MEM_SIZE, sizeof(cl_ulong), &lm, NULL);
//     if (err != CL_SUCCESS){
//         printf("Error getting device info\n");
//         return (-1);
//     }
//     //printf("Local Mem: %i\n",lm);
//
//     cl_uint mcl,mcm;
//     clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_MAX_CLOCK_FREQUENCY, sizeof(cl_uint), &mcl, NULL);
//     clGetDeviceInfo(deviceID[dev_number], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &mcm, NULL);
//     float card_tflops = mcl*1e6 * mcm*16*4*2 / 1e12;
//
//     // 3. Create a context and command queues on that device.
//     cl_context context = clCreateContext( NULL, 1, &deviceID[dev_number], NULL, NULL, NULL);
//     cl_command_queue queue[N_QUEUES];
//     for (int i = 0; i < N_QUEUES; i++){
//         queue[i] = clCreateCommandQueue( context, deviceID[dev_number], CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE | CL_QUEUE_PROFILING_ENABLE, &err );
//         //add a more robust error check at some point?
//         if (err){ //success returns a 0
//             printf("Error initializing queues.  Exiting program.\n");
//             return (-1);
//         }
//
//     }
//
//     // 4. Perform runtime source compilation, and obtain kernel entry point.
//     int size1_block = 32;
//     int num_blocks = (NUM_ELEM / size1_block) * (NUM_ELEM / size1_block + 1) / 2.; // 256/32 = 8, so 8 * 9/2 (= 36) //needed for the define statement
//
//     // 4a load the source files //this load routine is based off of example code in OpenCL in Action by Matthew Scarpino
//     char cl_fileNames[3][256];
//     sprintf(cl_fileNames[0],OPENCL_FILENAME_1);
//
//     if (XOR_MANUAL)
//         sprintf(cl_fileNames[1],OPENCL_FILENAME_2b);
//     else
//         sprintf(cl_fileNames[1],OPENCL_FILENAME_2);
//     sprintf(cl_fileNames[2],OPENCL_FILENAME_3);
//
//     char cl_options[1024];
//     sprintf(cl_options,"-g -D NUM_GATINGGROUPS=%d -D NUM_DATASETS=%d -D ACTUAL_NUM_ELEMENTS=%du -D ACTUAL_NUM_FREQUENCIES=%du -D NUM_ELEMENTS=%du -D NUM_FREQUENCIES=%du -D NUM_BLOCKS=%du -D NUM_TIMESAMPLES=%du -D BASE_TIMESAMPLES_INT_ACCUM=%d", NUM_GATING_GROUPS, NUM_DATA_SETS, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_ELEM, NUM_FREQ, num_blocks, NUM_TIMESAMPLES, BASE_TIMESAMPLES_ACCUM);
//     printf("Compiler Options: -g \n-D NUM_GATINGGROUPS=%d \n-D NUM_DATASETS=%d \n-D ACTUAL_NUM_ELEMENTS=%du \n-D ACTUAL_NUM_FREQUENCIES=%du \n-D NUM_ELEMENTS=%du \n-D NUM_FREQUENCIES=%du \n-D NUM_BLOCKS=%du \n-D NUM_TIMESAMPLES=%du \n-D BASE_TIMESAMPLES_INT_ACCUM=%d \n", NUM_GATING_GROUPS, NUM_DATA_SETS, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_ELEM, NUM_FREQ, num_blocks, NUM_TIMESAMPLES, BASE_TIMESAMPLES_ACCUM);
//
//     size_t cl_programSize[NUM_CL_FILES];
//     FILE *fp;
//     char *cl_programBuffer[NUM_CL_FILES];
//
//
//     for (int i = 0; i < NUM_CL_FILES; i++){
//         fp = fopen(cl_fileNames[i], "r");
//         if (fp == NULL){
//             printf("error loading file: %s\n", cl_fileNames[i]);
//             return (-1);
//         }
//         fseek(fp, 0, SEEK_END);
//         cl_programSize[i] = ftell(fp);
//         rewind(fp);
//         cl_programBuffer[i] = (char*)malloc(cl_programSize[i]+1);
//         cl_programBuffer[i][cl_programSize[i]] = '\0';
//         int sizeRead = fread(cl_programBuffer[i], sizeof(char), cl_programSize[i], fp);
//         if (sizeRead < cl_programSize[i])
//             printf("Error reading the file!!!");
//         fclose(fp);
//     }
//
//     cl_program program = clCreateProgramWithSource( context, NUM_CL_FILES, (const char**)cl_programBuffer, cl_programSize, &err );
//     if (err){
//         printf("Error in clCreateProgramWithSource: %i\n",err);
//         return(-1);
//     }
//
//     //printf("here1\n");
//     err = clBuildProgram( program, 1, &deviceID[dev_number], cl_options, NULL, NULL );
//     if (err){
//         printf("Error in clBuildProgram: %i\n\n",err);
//         size_t log_size;
//         clGetProgramBuildInfo(program, deviceID[dev_number], CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
//         char * program_log = (char*)malloc(log_size+1);
//         program_log[log_size] = '\0';
//         clGetProgramBuildInfo(program, deviceID[dev_number], CL_PROGRAM_BUILD_LOG, log_size+1, program_log, NULL);
//         printf("%s\n",program_log);
//         free(program_log);
//         return(-1);
//     }
//
//     cl_kernel offsetAccumulate_kernel = clCreateKernel( program, "offsetAccumulateElements", &err );
//     if (err){
//         printf("Error in clCreateKernel: %i\n\n",err);
//         return -1;
//     }
//     /* parameters for the kernel
//     void offsetAccumulateElements (__global const uint *inputData,
//                                         __global uint *outputData,
//                                         __global uint *counters,
//                                         __global const uint gating_group_count, //number of gating periods to check
//                                         __global const uint *num_time_bins, //number of bins to sort things into (part of duty cycle)
//                                         __global const uint *offset_remainder, //a 'wrapped' offset time
//                                         __global const uint *delta_time_bin, //time for 1 bin in the gating (in units of 10 ns)
//                                         __global const uint *bin_cut) //number of bins to keep in the 'on' section (entry values must be >= 1)
//      */
//
//     cl_kernel preseed_kernel = clCreateKernel( program, "preseed", &err );
//     if (err){
//         printf("Error in clCreateKernel: %i\n\n",err);
//         return -1;
//     }
//     /*
//     void preseed( __global const uint *dataIn,
//               __global  int *corr_buf,
//               __global const uint *id_x_map,
//               __global const uint *id_y_map,
//               __local  uint *localDataX,
//               __local  uint *localDataY,
//               __global const uint *counters)
//      */
//
//     cl_kernel corr_kernel = clCreateKernel( program, "corr", &err );
//     if (err){
//         printf("Error in clCreateKernel: %i\n\n",err);
//         return (-1);
//     }
//     /*
//     void corr ( __global const uint *packed,
//             __global  int *corr_buf,
//             __global const uint *id_x_map,
//             __global const uint *id_y_map,
//             __local  uint *stillPacked,
//             __global const uint gating_group_count, // <--
//             __global const unsigned int *num_time_bins, // <--
//             __global const unsigned int *offset_remainder, // <--
//             __global const unsigned int *delta_time_bin, // <--
//             __global const uint *bin_cut) // <--
//     */
//
//     for (int i =0; i < NUM_CL_FILES; i++){
//         free(cl_programBuffer[i]);
//     }
//
//     // 5. set up arrays and initilize if required
//     cl_mem device_bin_counters          [N_STAGES];
//     unsigned char *host_PrimaryInput    [N_STAGES]; //where things are brought from, ultimately. Code runs fastest when we create the aligned memory and then pin it to the device
//     //unsigned char *host_CLinput_data    [N_STAGES]; //for the pointer that is created when you map the host to the cl memory
//     int *host_PrimaryOutput             [N_STAGES];
//     //int *host_CLoutput_data             [N_STAGES];
//     cl_mem device_CLinput_pinnedBuffer  [N_STAGES];
//     cl_mem device_CLoutput_pinnedBuffer [N_STAGES];
//     cl_mem device_CLinput_kernelData    [N_STAGES];
//     cl_mem device_CLoutput_kernelData   [N_STAGES];
//
//     cl_mem device_CLoutputAccum         [N_STAGES];
//     //cl_mem device_CLoutputAccum_pinnedBuffer;
//     //cl_int *host_outputAccum; //to do: clean up after pointers
//
//
//     int len=NUM_FREQ*num_blocks*(size1_block*size1_block)*2*NUM_DATA_SETS;// *2 real and imag
//     printf("Num_blocks %d ", num_blocks);
//     printf("Output Length %d and size %ld B\n", len, len*sizeof(cl_int));
//     cl_int *zeros=calloc(len,sizeof(cl_int)); //for the output buffers
//
//     //posix_memalign ((void **)&host_CLinput_data, 4096, NUM_TIMESAMPLES*NUM_ELEM); //online it said that memalign was obsolete, so am using posix_memalign instead.  This should allow for pinning if desired.
//     //they use getpagesize() for the demo's alignment size for Linux for Firepro, rather than the 4096 which was mentioned in the BufferBandwidth demo.
//     //memalign taken care of by CL?
//
//     printf("Size of Input data = %i B\n", NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
//     printf("Number of Data Sets (for output) = %i\n", NUM_DATA_SETS);
//     printf("Total Data size (input) = %d B\n", NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
//
//     // Set up arrays so that they can be used later on
//     for (int i = 0; i < N_STAGES; i++){
//         //preallocate memory for pinned buffers
//         err = posix_memalign ((void **)&host_PrimaryInput[i], PAGESIZE_MEM, NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
//         //check if an extra command is needed to pre pin this--this might just make sure it is
//         //aligned in memory space.
//         if (err){
//             printf("error in creating memory buffers: Inputa, stage: %i, err: %i. Exiting program.\n",i, err);
//             return (err);
//         }
//         err = mlock(host_PrimaryInput[i], NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
//         if (err){
//             printf("error in creating memory buffers: Inputb, stage: %i, err: %i. Exiting program.\n",i, err);
//             printf("%s",strerror(errno));
//             return (err);
//         }
//
//         device_CLinput_pinnedBuffer[i] = clCreateBuffer ( context,
//                                     CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,//
//                                     NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ,
//                                     host_PrimaryInput[i],
//                                     &err); //create the clBuffer, using pre-pinned host memory
//
//         if (err){
//             printf("error in mapping pin pointers. Exiting program.\n");
//             return (err);
//         }
//
//         err = posix_memalign ((void **)&host_PrimaryOutput[i], PAGESIZE_MEM, len*sizeof(cl_int));
//         err |= mlock(host_PrimaryOutput[i],len*sizeof(cl_int));
//         if (err){
//             printf("error in creating memory buffers: Output, stage: %i. Exiting program.\n",i);
//             return (err);
//         }
//
//         device_CLoutput_pinnedBuffer[i] = clCreateBuffer (context,
//                                     CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR,
//                                     len*sizeof(cl_int),
//                                     host_PrimaryOutput[i],
//                                     &err); //create the output buffer and allow cl to allocate host memory
//
//         if (err){
//             printf("error in mapping pin pointers. Exiting program.\n");
//             return (err);
//         }
//
//         if (XOR_MANUAL){
//             device_CLinput_kernelData[i] = clCreateBuffer (context,
//                                         CL_MEM_READ_WRITE,// | CL_MEM_USE_PERSISTENT_MEM_AMD, //ran out of memory when I tried to use this
//                                         NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ,
//                                         0,
//                                         &err); //cl memory that can only be read by kernel
//
//             if (err){
//                 printf("error in allocating memory. Exiting program.\n");
//                 return (err);
//             }
//         }
//         else{
//             device_CLinput_kernelData[i] = clCreateBuffer (context,
//                                         CL_MEM_READ_ONLY,// | CL_MEM_USE_PERSISTENT_MEM_AMD, //ran out of memory when I tried to use this
//                                         NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ,
//                                         0,
//                                         &err); //cl memory that can only be read by kernel
//
//             if (err){
//                 printf("error in allocating memory. Exiting program.\n");
//                 return (err);
//             }
//         }
//
//         device_CLoutput_kernelData[i] = clCreateBuffer (context,
//                                     CL_MEM_WRITE_ONLY|CL_MEM_COPY_HOST_PTR,
//                                     len*sizeof(cl_int),
//                                     zeros,
//                                     &err); //cl memory that can only be written to by kernel--preset to 0s everywhere
//
//         if (err){
//             printf("error in allocating memory. Exiting program.\n");
//             return (err);
//         }
//
//         device_bin_counters[i] = clCreateBuffer(context,
//                                 CL_MEM_READ_WRITE,
//                                 NUM_DATA_SETS*sizeof(cl_uint),
//                                 0,
//                                 &err);
//         if (err){
//             printf("error in allocating memory. Exiting program.\n");
//             return (err);
//         }
//
//     } //end for
//     free(zeros);
//
//     //initialize an array for the accumulator of offsets (borrowed this buffer from an old version of code--check this)
// ///////
//     zeros=calloc(NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS,sizeof(cl_uint)); // <--this was missed! Was causing most of the problems!
//     cl_uint *zeros2 = calloc(NUM_DATA_SETS,sizeof(cl_uint));
// //////
//     device_CLoutputAccum[0] = clCreateBuffer(context,
//                                           CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
//                                           NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(cl_uint),
//                                           zeros,
//                                           &err);
//     if (err){
//             printf("error in allocating memory. Exiting program.\n");
//             return (err);
//     }
//
//     device_CLoutputAccum[1] = clCreateBuffer(context,
//                                           CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
//                                           NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(cl_uint),
//                                           zeros,
//                                           &err);
//     if (err){
//             printf("error in allocating memory. Exiting program.\n");
//             return (err);
//     }
//
//     //arrays have been allocated
//
//     //--------------------------------------------------------------
//     //Generate Data Set!
//
//     generate_char_data_set(GEN_TYPE,
//                            GEN_DEFAULT_SEED, //random seed
//                            GEN_DEFAULT_RE,//default_real,
//                            GEN_DEFAULT_IM,//default_imaginary,
//                            GEN_INITIAL_RE,//initial_real,
//                            GEN_INITIAL_IM,//initial_imaginary,
//                            GEN_FREQ,//int single_frequency,
//                            NUM_TIMESAMPLES,//int num_timesteps,
//                            ACTUAL_NUM_FREQ,//int num_frequencies,
//                            ACTUAL_NUM_ELEM,//int num_elements,
//                            1,//NUM_DATA_SETS, need a 1 here because we have 1 long input array and many output arrays, NOT multiple input AND output arrays
//                            host_PrimaryInput[0]);
//
//     if (NUM_ELEM <=32){
//         if (INTERLEAVED){
//             reorder_data_interleave_2_frequencies(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS,host_PrimaryInput[0]);
//         }
//     }
//     //print_element_data(1, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, ALL_FREQUENCIES, host_PrimaryInput[0]);
//     //reorder_data_phaseB_breakData(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, host_PrimaryInput[0]);
//
//     memcpy(host_PrimaryInput[1], host_PrimaryInput[0], NUM_TIMESAMPLES*NUM_ELEM*NUM_FREQ);
//
//     //--------------------------------------------------------------
//
//
//     // 6. Set up Kernel parameters
//
//     //upper triangular address mapping --converting 1d addresses to 2d addresses
//     unsigned int global_id_x_map[num_blocks];
//     unsigned int global_id_y_map[num_blocks];
//
// //     for (int i=0; i<num_blocks; i++){
// //         int t = (int)(sqrt(1 + 8*(num_blocks-i-1))-1)/2; /*t is number of the current row, counting/increasing row numbers from the bottom, up, and starting at 0 --note it uses the property that converting to int uses a floor/truncates at the decimal*/
// //         int y = NUM_ELEM/size1_block-t-1;
// //         int x = (t+1)*(t+2)/2 + (i - num_blocks)+y;
// //         global_id_x_map[i] = x;
// //         global_id_y_map[i] = y;
// //         printf("i = %d: t = %d, y = %d, x = %d \n", i, t, y, x);
// //     }
//
//     //TODO: p260 OpenCL in Action has a clever while loop that changes 1 D addresses to X & Y indices for an upper triangle.  Time Test kernels using
//     //them compared to the lookup tables for NUM_ELEM = 256
//     int largest_num_blocks_1D = NUM_ELEM/size1_block;
//     int index_1D = 0;
//     for (int j = 0; j < largest_num_blocks_1D; j++){
//         for (int i = j; i < largest_num_blocks_1D; i++){
//             global_id_x_map[index_1D] = i;
//             global_id_y_map[index_1D] = j;
//             index_1D++;
//         }
//     }
//
//     cl_mem id_x_map = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
//                     num_blocks * sizeof(cl_uint), global_id_x_map, &err);
//     if (err){
//         printf("Error in clCreateBuffer %i\n", err);
//     }
//
//     cl_mem id_y_map = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
//                     num_blocks * sizeof(cl_uint), global_id_y_map, &err);
//     if (err){
//         printf("Error in clCreateBuffer %i\n", err);
//     }
//
//     cl_uint num_gate_groups = NUM_GATING_GROUPS;
//     cl_uint gate_size_time [NUM_GATING_GROUPS];
//     cl_uint num_time_bins [NUM_GATING_GROUPS];
//     cl_uint offset_remainder [NUM_GATING_GROUPS];
//     cl_uint clip_bin [NUM_GATING_GROUPS];
//
//     for (int i = 0 ; i < NUM_GATING_GROUPS; i++){
//         gate_size_time[i] = GATE_PERIOD_IN_10ns_UNITS;
//         num_time_bins [i] = 128;
//         clip_bin [i]      = 1;
//         offset_remainder[i] = 0; //default values...
//     }
//
//     cl_mem device_gate_size_time = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS*sizeof(cl_uint),gate_size_time, &err);
//     cl_mem device_num_time_bins = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS *sizeof(cl_uint),num_time_bins,&err);
//     cl_mem device_clip_bin = clCreateBuffer(context,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, NUM_GATING_GROUPS * sizeof(cl_uint), clip_bin,&err);
//     cl_mem device_offset_remainder = clCreateBuffer(context,CL_MEM_READ_WRITE, NUM_GATING_GROUPS*sizeof(cl_uint), NULL, &err);
//
//
//
//     //set other parameters that will be fixed for the kernels (changeable parameters will be set in run loops)
//     /* parameters for the kernel
//     void offsetAccumulateElements (__global const uint *inputData,              //0 <--
//                                         __global uint *outputData,              //1 <--
//                                         __global uint *counters,                //2 <--
//                                         __global const uint gating_group_count, //3 number of gating periods to check
//                                         __global const uint *num_time_bins,     //4 number of bins to sort things into (part of duty cycle)
//                                         __global const uint *offset_remainder,  //5 <-- a 'wrapped' offset time
//                                         __global const uint *delta_time_bin,    //6 <-- time for 1 bin in the gating (in units of 10 ns)
//                                         __global const uint *bin_cut)           //7 number of bins to keep in the 'on' section (entry values must be >= 1)
//      */
//     clSetKernelArg(offsetAccumulate_kernel, 3, sizeof(cl_uint),&num_gate_groups);
//     clSetKernelArg(offsetAccumulate_kernel, 4, sizeof(void *),(void *)&device_num_time_bins);
//     clSetKernelArg(offsetAccumulate_kernel, 6, sizeof(void *),(void *)&device_gate_size_time);
//     clSetKernelArg(offsetAccumulate_kernel, 7, sizeof(void *),(void *)&device_clip_bin);
//
//
//     /*
//     void preseed( __global const uint *dataIn,  //0 <--
//               __global  int *corr_buf,          //1 <--
//               __global const uint *id_x_map,    //2
//               __global const uint *id_y_map,    //3
//               __local  uint *localDataX,        //4
//               __local  uint *localDataY,        //5
//               __global const uint *counters)    //6 <--
//      */
//     clSetKernelArg(preseed_kernel, 2, sizeof(id_x_map), (void*) &id_x_map); //this should maybe be sizeof(void *)?
//     clSetKernelArg(preseed_kernel, 3, sizeof(id_y_map), (void*) &id_y_map);
//     clSetKernelArg(preseed_kernel, 4, 64* sizeof(cl_uint), NULL);
//     clSetKernelArg(preseed_kernel, 5, 64* sizeof(cl_uint), NULL);
//
//      /*
//     void corr ( __global const uint *packed,                //0 <--
//             __global  int *corr_buf,                        //1 <--
//             __global const uint *id_x_map,                  //2
//             __global const uint *id_y_map,                  //3
//             __local  uint *stillPacked,                     //4
//             __global const uint gating_group_count,         //5
//             __global const unsigned int *num_time_bins,     //6
//             __global const unsigned int *offset_remainder,  //7 <--
//             __global const unsigned int *delta_time_bin,    //8
//             __global const uint *bin_cut)                   //9
//     */
//     clSetKernelArg(corr_kernel, 2, sizeof(id_x_map), (void*) &id_x_map); //this should maybe be sizeof(void *)?
//     clSetKernelArg(corr_kernel, 3, sizeof(id_y_map), (void*) &id_y_map);
//     clSetKernelArg(corr_kernel, 4, 8*8*4 * sizeof(cl_uint), NULL); //define the size of the local memory///TODO--check if hardcoding has any speed effects
//     clSetKernelArg(corr_kernel, 5, sizeof(cl_uint), &num_gate_groups);
//     clSetKernelArg(corr_kernel, 6, sizeof(void *), &device_num_time_bins);
//     clSetKernelArg(corr_kernel, 8, sizeof(void *), &device_gate_size_time);
//     clSetKernelArg(corr_kernel, 9, sizeof(void *), &device_clip_bin);
//
//     //uint data_input_length = ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ*NUM_TIMESAMPLES/4;
//     //clSetKernelArg(offsetAccumulate_kernel, 2, sizeof(data_input_length),&data_input_length);
//
//     unsigned int n_cAccum=NUM_TIMESAMPLES/256u; //n_cAccum == number_of_compressedAccum
//     size_t gws_accum[3]={64, (int)ceil(NUM_ELEM*NUM_FREQ/256.0),NUM_TIMESAMPLES/BASE_TIMESAMPLES_ACCUM}; //1024 is the number of iterations performed in the kernel--hardcoded, so if the number in the kernel changes, change here as well
//     size_t lws_accum[3]={64, 1, 1};
//
//     size_t gws_preseed[3]={8*NUM_DATA_SETS, 8*NUM_FREQ, num_blocks};
//     size_t lws_preseed[3]={8, 8, 1};
//
//     size_t gws_corr[3]={8,8*NUM_FREQ,num_blocks*n_cAccum}; //global work size array
//     size_t lws_corr[3]={8,8,1}; //local work size array
//     //int *corr_ptr;
//
//     //setup and start loop to process data in parallel
//     int spinCount = 0; //we rotate through values to launch processes in order for the command queues. This helps keep track of what position, and can run indefinitely without overflow
//     int writeToDevStageIndex;
//     int kernelStageIndex;
//     //int readFromDevStageIndex;
//
//     cl_int numWaitEventWrite = 0;
//     cl_event* eventWaitPtr = NULL;
//     cl_event clearCountersEvent;
//     cl_event copy_time_remainders_event;
//
//     cl_event lastWriteEvent[N_STAGES]  = { 0 }; // All entries initialized to 0, since unspecified entries are set to 0
//     cl_event lastKernelEvent[N_STAGES] = { 0 };
//     //cl_event lastReadEvent[N_STAGES]   = { 0 };
//     cl_event copyInputDataEvent;
//     cl_event offsetAccumulateEvent;
//     cl_event preseedEvent;
//
//     if (TIMER_FOR_PROCESSING_ONLY){
//         for (int i = 0; i < N_STAGES; i++){
//              err = clEnqueueWriteBuffer(queue[0],
//                                     device_CLinput_kernelData[i], //to here
//                                     CL_TRUE,
//                                     0, //offset
//                                     NUM_TIMESAMPLES * NUM_ELEM*NUM_FREQ, //8 for multifreq interleaving
//                                     host_PrimaryInput[i], //from here
//                                     0,
//                                     NULL,
//                                     NULL);
//             if (err){
//                 printf("Error in transfer to device memory. Error in loop %d, error: %s\n",i,oclGetOpenCLErrorCodeStr(err));
//                 exit(err);
//             }
//         }
//         clFinish(queue[0]);
//         printf("copy complete\n");
//     }
//
//     ///////////////////////////////////////////////////////////////////////////////
//     cputime = e_time();
//     for (int i=0; i<=nkern; i++){//if we were truly streaming data, for each correlation, we would need to change what arrays are used for input/output
//         //printf("spinCount %d\n",spinCount);
//         writeToDevStageIndex =  (spinCount ); // + 0) % N_STAGES;
//         kernelStageIndex =      (spinCount + 1 ) % N_STAGES; //had been + 2 when it was 3 stages
//         //readFromDevStageIndex = (spinCount + 1 ) % N_STAGES;
//
//         //transfer section
//         if (i < nkern){ //Start at 0, Stop before the last loop
//             //check if it needs to wait on anything
//             if(lastKernelEvent[writeToDevStageIndex] != 0){ //only equals 0 when it hasn't yet been defined i.e. the first run through the loop with N_STAGES == 2
//                 numWaitEventWrite = 1;
//                 eventWaitPtr = &lastKernelEvent[writeToDevStageIndex]; //writes must wait on the last kernel operation since
//             }
//             else {
//                 numWaitEventWrite = 0;
//                 eventWaitPtr = NULL;
//                 }
//
//             //copy necessary buffers to device memory
//             if (TIMER_FOR_PROCESSING_ONLY){
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_CLoutputAccum[writeToDevStageIndex],
//                                         CL_FALSE,
//                                         0,
//                                         NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(cl_int),
//                                         zeros,
//                                         numWaitEventWrite,
//                                         eventWaitPtr,
//                                         &clearCountersEvent);
//
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_offset_remainder,
//                                         CL_FALSE,
//                                         0,
//                                         NUM_GATING_GROUPS*sizeof(cl_uint),
//                                         offset_remainder,
//                                         1,
//                                         &clearCountersEvent,
//                                         &copy_time_remainders_event);
//
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_bin_counters[writeToDevStageIndex],
//                                         CL_FALSE,
//                                         0,
//                                         NUM_DATA_SETS*sizeof(cl_uint),
//                                         zeros2,
//                                         1,
//                                         &copy_time_remainders_event,
//                                         &lastWriteEvent[writeToDevStageIndex]);
//
//
//                 err = clFlush(queue[0]);
//                 if (err){
//                     printf("Error in flushing transfer to device memory. Error in loop %d\n",i);
//                     exit(err);
//                 }
// //                 if (i%100 == 0)
// //                     printf(".");
//             }
//             else{
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_CLinput_kernelData[writeToDevStageIndex], //to here
//                                         CL_FALSE,
//                                         0, //offset
//                                         NUM_TIMESAMPLES * NUM_ELEM*NUM_FREQ, //
//                                         host_PrimaryInput[writeToDevStageIndex], //from here
//                                         numWaitEventWrite,
//                                         eventWaitPtr,
//                                         &clearCountersEvent);
//                 //printf(".");
//                 uint wrapped_time_offset = 0; //should be the current_time%(time_per_bin*NUM_DATA_SETS)
//                 for (int g_group = 0; g_group <= num_gate_groups; g_group++){
//                     offset_remainder[g_group] = wrapped_time_offset;
//                 }
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_bin_counters[writeToDevStageIndex],
//                                         CL_FALSE,
//                                         0,
//                                         NUM_DATA_SETS*sizeof(cl_uint),
//                                         zeros2,
//                                         1,
//                                         &clearCountersEvent,
//                                         &copyInputDataEvent);
//                 if (err){
//                     printf("Error in transfer to device memory. Error in loop %d, error: %s\n",i,oclGetOpenCLErrorCodeStr(err));
//                     exit(err);
//                 }
//
//                 err = clEnqueueWriteBuffer(queue[0],
//                                         device_CLoutputAccum[writeToDevStageIndex],
//                                         CL_FALSE,
//                                         0,
//                                         NUM_FREQ*NUM_ELEM*2*NUM_DATA_SETS*sizeof(cl_int),
//                                         zeros,
//                                         1,
//                                         &copyInputDataEvent,
//                                         &lastWriteEvent[writeToDevStageIndex]);
//
//
//                 err = clFlush(queue[0]);
//                 if (err){
//                     printf("Error in flushing transfer to device memory. Error in loop %d\n",i);
//                     exit(err);
//                 }
//             }
//         }
//         //printf("hello");
//
//         //processing section
//         if (lastWriteEvent[kernelStageIndex] !=0 && i <= nkern){//insert additional steps for processing here
//             //required steps include: offset accumulator (order(Num_elements*Num_frequencies*Num_timesteps))
//             //then pre-seed output array
//             //then perform the correlation
//
//             //accumulateFeeds_kernel--set 2 arguments--input array and zeroed output array
//             err = clSetKernelArg(offsetAccumulate_kernel,
//                                  0,
//                                  sizeof(void*),
//                                  (void*) &device_CLinput_kernelData[kernelStageIndex]);
//
//             err |= clSetKernelArg(offsetAccumulate_kernel,
//                                   1,
//                                   sizeof(void *),
//                                   (void *) &device_CLoutputAccum[kernelStageIndex]); //make sure this array is zeroed initially!
//             err |= clSetKernelArg(offsetAccumulate_kernel,
//                                   2,
//                                   sizeof(void *),
//                                   (void *) &device_bin_counters[kernelStageIndex]);
//
//
//
//             err |= clSetKernelArg(offsetAccumulate_kernel,
//                                   5,
//                                   sizeof(void *),
//                                   &device_offset_remainder);
// //             err |= clSetKernelArg(offsetAccumulate_kernel,
// //                                   7,
// //                                   sizeof(void *),
// //                                   (void *)&clip_bin);
//             if (err){
//                 printf("Error setting the 0th kernel arguments in loop %d\n", i);
//                 exit(err);
//             }
//             //else{
//             //    printf("ok\n");
//             //}
//
//             err = clEnqueueNDRangeKernel(queue[1],
//                                          offsetAccumulate_kernel,
//                                          3,
//                                          NULL,
//                                          gws_accum,
//                                          lws_accum,
//                                          1,
//                                          &lastWriteEvent[kernelStageIndex], // make sure data is present, first
//                                          &offsetAccumulateEvent);
//             if (err){
//                 printf("Error accumulating in loop %d\n", i);
//                 exit(err);
//             }
//
//             //preseed_kernel--set only 3 of the 7 arguments (the other 4 stay the same)
//             err = clSetKernelArg(preseed_kernel,
//                                  0,
//                                  sizeof(void *),
//                                  (void *) &device_CLoutputAccum[kernelStageIndex]);//assign the accumulated data as input
//
//             err |= clSetKernelArg(preseed_kernel,
//                                  1,
//                                  sizeof(void *),
//                                  (void *) &device_CLoutput_kernelData[kernelStageIndex]); //set the output for preseeding the correlator array
//
//             err |= clSetKernelArg(preseed_kernel,
//                                  6,
//                                  sizeof(void *),
//                                  (void *) &device_bin_counters[kernelStageIndex]); //set the output for preseeding the correlator array
//
//             if (err){
//                 printf("Error setting the 1st kernel arguments in loop %d\n", i);
//                 exit(err);
//             }
//
//             err = clEnqueueNDRangeKernel(queue[1],
//                                          preseed_kernel,
//                                          3, //3d global dimension, also worksize
//                                          NULL, //no offsets
//                                          gws_preseed,
//                                          lws_preseed,
//                                          1,
//                                          &offsetAccumulateEvent,//dependent on previous step so don't use &lastWriteEvent[kernelStageIndex],
//                                          &preseedEvent);
//             if (err){
//                 printf("Error performing preseed kernel operation in loop %d: error %d\n", i,err);
//                 exit(err);
//             }
//
//             //corr_kernel--set the input and output buffers (the other parameters stay the same).
//             err =  clSetKernelArg(corr_kernel,
//                                     0,
//                                     sizeof(device_CLinput_kernelData[kernelStageIndex]), //sizeof(void *)
//                                     (void*) &device_CLinput_kernelData[kernelStageIndex]);
//
//             err |= clSetKernelArg(corr_kernel,
//                                     1,
//                                     sizeof(void *),//sizeof(void *)
//                                     (void*) &device_CLoutput_kernelData[kernelStageIndex]);
//
//
//             err |= clSetKernelArg(corr_kernel,
//                                     7,
//                                     sizeof(void *),
//                                     &device_offset_remainder);
//
//
//             if (err){
//                 printf("Error setting the 2nd kernel arguments in loop %d\n", i);
//                 exit(err);
//             }
//
//             err = clEnqueueNDRangeKernel(queue[1],
//                                          corr_kernel,
//                                          3, //3d global dimension, also worksize
//                                          NULL, //no offsets
//                                          gws_corr,
//                                          lws_corr,
//                                          1,
//                                          &preseedEvent,//&lastWriteEvent[kernelStageIndex],//&preseedEvent,//dependent on previous step so don't use &lastWriteEvent[kernelStageIndex],
//                                          &lastKernelEvent[kernelStageIndex]);
//             if (err){
//                 printf("Error performing corr kernel operation in loop %d, err: %d\n", i,err);
//                 exit(err);
//             }
//             //printf("kernelStageIndex %i\n", kernelStageIndex);
//             //if (kernelStageIndex == 1)
//             //  err = clFinish(queue[1]);
//             //else
//                 err = clFlush(queue[1]);
//             if (err){
//                 printf("Error in flushing kernel run. Error in loop %d\n",i);
//                 exit(err);
//             }
//
//         }
//
//         //since the kernel accumulates results, it isn't necessary/wanted to have it pull out results each time
//         //processing of the results would need to be done, arrays reset (or kernel changed); the transfers could also slow things down, too....
//         if (i%10==0){
//             err =  clFinish(queue[0]);
//             err |= clFinish(queue[1]);
//
//             if (err){
//                 printf("Error while finishing up the queue after the loops.\n");
//                 return (err);
//             }
//         }
//         spinCount++;
//         spinCount = (spinCount < N_STAGES) ? spinCount : 0; //keeps the value of spinCount small, always, and then saves 1 remainder calculation earlier in the loop.
//     }
//
//     //since there are only 2, simplify things (i.e. no need for a loop).
//     err =  clFinish(queue[0]);
//     err |= clFinish(queue[1]);
//
//     if (err){
//         printf("Error while finishing up the queue after the loops.\n");
//         return (err);
//     }
//  //printf("HELLO\n");
//     clReleaseEvent(preseedEvent);
//     clReleaseEvent(copyInputDataEvent);
//     clReleaseEvent(offsetAccumulateEvent);
//     clReleaseEvent(lastKernelEvent[0]);
//     clReleaseEvent(lastKernelEvent[1]);
//     clReleaseEvent(lastWriteEvent[0]);
//     clReleaseEvent(lastWriteEvent[1]);
//     // 7. Look at the results via synchronous buffer map.
//     //int *corr_ptr;
//     //printf(".");
//     err = clEnqueueReadBuffer(queue[0], device_CLoutput_kernelData[0], CL_TRUE, 0, len*sizeof(cl_int), host_PrimaryOutput[0], 0, NULL, NULL);
//     err |= clEnqueueReadBuffer(queue[0], device_CLoutput_kernelData[1], CL_TRUE, 0, len*sizeof(cl_int), host_PrimaryOutput[1], 0, NULL, NULL);
//
//     if (err){
//         printf("Error reading data back to host.\n");
//         //return (err);
//     }
//
//     err = clFinish(queue[0]);
//
//     if (err){
//         printf("Error while finishing up the queue after the loops.\n");
//         //return (err);
//     }
//
//     //printf("to transfer part 1\n");
//     //accumulate results into one array
//     //unmap output?
//
//     printf("Running %i iterations of full corr (%i time samples (%i Ki time samples), %i elements, %i frequencies, %i data set", nkern, NUM_TIMESAMPLES, NUM_TIMESAMPLES/1024, ACTUAL_NUM_ELEM, ACTUAL_NUM_FREQ, NUM_DATA_SETS);
//
//     if (NUM_DATA_SETS == 1)
//         printf(")\n");
//     else
//         printf("s)\n");
//
//     cputime = e_time()-cputime;
//     if (nkern > 1){
//         for (int i = 0; i < len; i++){
//             //dump out results
//             if (DEBUG){
//                 printf("%d ",host_PrimaryOutput[0][i]);
//                 if ((i+1) % (32 * 32* 2) == 0)
//                     printf("\n");
//             }
//             //host_CLoutput_data[0][i] += host_CLoutput_data[1][i];
//             host_PrimaryOutput[0][i] += host_PrimaryOutput[1][i];
//             host_PrimaryOutput[0][i] /=2; //the results in output 0 and 1 should be identical--this is just to check (in a rough way) that they are.
//             //if the average of the two arrays is the correct answer, and one expects both of them to have an answer, then the answers of both should be correct
//         }
//     }
//     //printf("to transfer part 2\n");
//     //--------------------------------------------------------------
//     //--------------------------------------------------------------
//     double unpack_Rate =(1.0*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern) /cputime/1000.0 ;// /cputime/1000.0;
//     printf("Unpacking rate: %6.4fs on GPU (%.1f kHz)\n",cputime,unpack_Rate);
//     printf("    [Theoretical max: @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
//                                     card_tflops*1e12 / (ACTUAL_NUM_ELEM/2.*(ACTUAL_NUM_ELEM+1.) * 2. * 2.) / 1e3,
//                                     100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (ACTUAL_NUM_ELEM/2.*(ACTUAL_NUM_ELEM+1.) * 2. * 2.)));
//     if (ACTUAL_NUM_ELEM == 16){
//         printf("    [Algorithm max:   @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
//                                     card_tflops*1e12 / (num_blocks * 16 * 16 * 2. * 2.) / 1e3,
//                                     100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (num_blocks * 16 * 16 * 2. * 2.)));
//     }
//     else{
//         printf("    [Algorithm max:   @%.1f TFLOPS, %.1f kHz; %2.0f%% efficiency]\n", card_tflops,
//                                     card_tflops*1e12 / (num_blocks * size1_block * size1_block * 2. * 2.) / 1e3,
//                                     100.*NUM_TIMESAMPLES*ACTUAL_NUM_FREQ*nkern/cputime / ((card_tflops*1e12) / (num_blocks * size1_block * size1_block * 2. * 2.)));
//
//     }
// /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// //continue search for NUM_DATA_SETS after this point--could cause bugs if misused
// /////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//     int *correlated_CPU;
//     int *correlated_GPU;
//     double *amp2_ratio_GPU_div_CPU;
//     double *phaseAngleDiff_GPU_m_CPU;
//     if (VERIFY_RESULTS){
//         // start using calls to do the comparisons
//         cputime = e_time();
//         correlated_CPU = calloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM))*ACTUAL_NUM_FREQ*2*NUM_DATA_SETS,sizeof(int)); //made for the largest possible size (one size fits all)
//         //int *correlated_CPU = calloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM+1))/2*ACTUAL_NUM_FREQ*2,sizeof(int));
//         if (correlated_CPU == NULL){
//             printf("failed to allocate memory\n");
//             return(-1);
//         }
//     //    err = cpu_data_generate_and_correlate(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, correlated_CPU);
//         //int cpu_data_generate_and_correlate_upper_triangle_only_gated_general(int num_timesteps, int num_frequencies, int num_elements, unsigned int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data_triangle, int num_gate_groups, unsigned int *time_offset_remainder, unsigned int *single_gate_period){
//         //int cpu_data_generate_and_correlate_gated_general(int num_timesteps, int num_frequencies, int num_elements, int *num_time_bins, unsigned int *bin_cutoff, int *correlated_data, int num_gate_groups, int *time_offset_remainder, int *single_gate_period){
//         if (UPPER_TRIANGLE){
//             err = cpu_data_generate_and_correlate_upper_triangle_only_gated_general(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, num_time_bins, clip_bin, correlated_CPU,NUM_GATING_GROUPS,offset_remainder,gate_size_time);
//         }
//         else{
//             err = cpu_data_generate_and_correlate_gated_general(NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, num_time_bins, clip_bin, correlated_CPU, NUM_GATING_GROUPS, offset_remainder,gate_size_time);
//         }
//
//         correlated_GPU = (int *)malloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM))*HDF5_FREQ*2*NUM_DATA_SETS*sizeof(int));
//         //int *correlated_GPU = (int *)malloc((ACTUAL_NUM_ELEM*(ACTUAL_NUM_ELEM+1))/2*ACTUAL_NUM_FREQ*2*sizeof(int));
//         if (correlated_GPU == NULL){
//             printf("failed to allocate memory\n");
//             return(-1);
//         }
//
//         if (ACTUAL_NUM_ELEM == 16){
//             //printf("Hey!\n");
//             if (INTERLEAVED){
//                 //printf("I was interleaved!\n");
//                 reorganize_32_to_16_feed_GPU_Correlated_Data_Interleaved(ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0]);
//             }
//             else{
//                 //printf("REORGANIZE!!!\n");
//                 reorganize_32_to_16_feed_GPU_Correlated_Data(ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0]); //needed for comparison of outputs.
//             }
//
//             if (UPPER_TRIANGLE){
//                 //printf("REORGANIZE!!\n");
//                 reorganize_data_16_element_with_triangle_conversion(HDF5_FREQ, ACTUAL_NUM_FREQ,NUM_DATA_SETS,host_PrimaryOutput[0],correlated_GPU);
//             }
//         }
//         else{
//             if (UPPER_TRIANGLE){
//                 reorganize_GPU_to_upper_triangle(size1_block, num_blocks, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0], correlated_GPU);
//             }
//             else{
//                 reorganize_GPU_to_full_Matrix_for_comparison(size1_block, num_blocks, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, host_PrimaryOutput[0], correlated_GPU);
//             }
//         }
//         //correct_GPU_correlation_results (NUM_TIMESAMPLES, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, host_PrimaryOutput[0], host_outputAccum); //host side correction not needed now.
//         int number_errors = 0;
//         int64_t errors_squared;
//         amp2_ratio_GPU_div_CPU = (double *)malloc(ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(double));
//         if (amp2_ratio_GPU_div_CPU == NULL){
//             printf("ran out of memory\n");
//             return (-1);
//         }
//         phaseAngleDiff_GPU_m_CPU = (double *)malloc(ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ*NUM_DATA_SETS*sizeof(double));
//         if (phaseAngleDiff_GPU_m_CPU == NULL){
//             printf("2ran out of memory\n");
//             return (-1);
//         }
//
//         if (UPPER_TRIANGLE){
//             compare_NSquared_correlator_results_data_has_upper_triangle_only ( &number_errors, &errors_squared, HDF5_FREQ, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, correlated_GPU, correlated_CPU, amp2_ratio_GPU_div_CPU, phaseAngleDiff_GPU_m_CPU, CHECKING_VERBOSE);
//         }
//         else{
//             compare_NSquared_correlator_results ( &number_errors, &errors_squared, ACTUAL_NUM_FREQ, ACTUAL_NUM_ELEM, NUM_DATA_SETS, correlated_GPU, correlated_CPU, amp2_ratio_GPU_div_CPU, phaseAngleDiff_GPU_m_CPU, CHECKING_VERBOSE);
//         }
//
//         if (number_errors > 0)
//             printf("Error with correlation/accumulation! Num Err: %d and length of correlated data: %d\n",number_errors, ACTUAL_NUM_ELEM*ACTUAL_NUM_ELEM*ACTUAL_NUM_FREQ);
//         else
//             printf("Correlation/accumulation successful! CPU matches GPU.\n");
//         //printf ("idx = %d\n", idx);
//         cputime=e_time()-cputime;
//         printf("Full Corr: %4.2fs on CPU (%.2f kHz)\n",cputime,NUM_TIMESAMPLES/cputime/1e3);
//     }
//     else{
//         printf("Results unverified.  Use this solely as a gpu processing rate indicator, and verify results before adopting all settings\n");
//     }
//     err = munlockall();
//
//
//     for (int ns=0; ns < N_STAGES; ns++){
//         err = clReleaseMemObject(device_CLinput_pinnedBuffer[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//         err = clReleaseMemObject( device_CLoutput_pinnedBuffer[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//         err = clReleaseMemObject(device_CLinput_kernelData[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//         err = clReleaseMemObject(device_CLoutput_kernelData[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//         err = clReleaseMemObject(device_bin_counters[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//         assert(host_PrimaryInput[ns]!=NULL);
//         free(host_PrimaryInput[ns]);
//         free(host_PrimaryOutput[ns]);
//
//         //err = clReleaseMemObject(device_CLoutputAccum_pinnedBuffer[ns]);
//         //if (err != SDK_SUCCESS) {
//         //    printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//         //    printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//         //    exit(err);
//         //}
//         err = clReleaseMemObject(device_CLoutputAccum[ns]);
//         if (err != SDK_SUCCESS) {
//             printf("clReleaseMemObject() failed with %d (%s)\n",err,oclGetOpenCLErrorCodeStr(err));
//             printf("Error at line %u in file %s !!!\n\n", __LINE__, __FILE__);
//             exit(err);
//         }
//     }
//
//
//     //free(corr_re);
//     //free(corr_im);
//     if (VERIFY_RESULTS){
//         free(correlated_CPU);
//         free(correlated_GPU);
//
//         //free(host_outputAccum);
//         free(amp2_ratio_GPU_div_CPU);
//         free(phaseAngleDiff_GPU_m_CPU);
//     }
//     //free(accum_re);
//     //free(accum_im);
//
//     free(zeros);
//     free(zeros2);
//     //--------------------------------------------------------------
//     //--------------------------------------------------------------
//
//
//     //free(data_block);
//     //free(input_data_2);
//     //free(output_data_1);
//     //free(output_data_2);
//
//     //err = clEnqueueUnmapMemObject(queue,corr_buffer,corr_ptr,0,NULL,NULL);
//     //if (err) printf("Error in clEnqueueUnmapMemObject!\n");
//     //clFinish(queue);
//
//     clReleaseKernel(corr_kernel);
//     clReleaseProgram(program);
//     //clReleaseMemObject(input_buffer);
//     //clReleaseMemObject(input_buffer2);
//     //clReleaseMemObject(copy_buffer);
//     //clReleaseMemObject(copy_buffer2);
//     //clReleaseMemObject(corr_buffer);
//     clReleaseMemObject(id_x_map);
//     clReleaseMemObject(id_y_map);
//     clReleaseCommandQueue(queue[0]);
//     clReleaseCommandQueue(queue[1]);
//     clReleaseContext(context);
//     return 0;
//}
