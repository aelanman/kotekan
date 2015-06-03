//CASPER ordering--actually, this kernel doesn't care about the original packed B data, so it should be the same as the version in the Non-casper directory
//Preseed the output array for correlations

//The data will be binned up (correction-wise) and the function here will create the proper corrections for the offsets.
//  * the counter from the offsetAccumulator counts in groups of 256

//HARDCODE ALL OF THESE!
//NUM_ELEMENTS, NUM_BLOCKS, NUM_TIMESAMPLES defined at compile time
//#define NUM_ELEMENTS                    32u // must be 32 or larger, 2560u eventually
//#define NUM_BLOCKS                      1u  // N(N+1)/2 where N=(NUM_ELEMENTS/32)
//#define NUM_TIMESAMPLES                 10u*1024u


//#define NUM_TIMESAMPLES_x_128           (NUM_TIMESAMPLES*128u)// need the total number of iterations for the offset outputs
#define NUM_BLOCKS_x_2048               (NUM_BLOCKS*2048u) //each block size is 32 x 32 x 2 = 2048

#define LOCAL_SIZE                      8u
#define BLOCK_DIM                       32u

#define DATA_SET_INDEX                  (get_group_id(0))
#define FREQUENCY_BAND                  (get_group_id(1))
#define BLOCK_ID                        (get_group_id(2))
#define LOCAL_X                         (get_local_id(0))
#define LOCAL_Y                         (get_local_id(1))


__kernel __attribute__((reqd_work_group_size(LOCAL_SIZE, LOCAL_SIZE, 1)))
void preseed( __global const uint *dataIn,
              __global  int *corr_buf,
              __global const uint *id_x_map,
              __global const uint *id_y_map,
              __global const uint *counters)
{
    __local uint localDataX[64];
    __local uint localDataY[64];
    uint block_x = id_x_map[BLOCK_ID]; //column of output block
    uint block_y = id_y_map[BLOCK_ID]; //row of output block  //if NUM_BLOCKS = 1, then BLOCK_ID = 0 then block_x = block_y = 0

    uint local_index = LOCAL_X + LOCAL_Y*LOCAL_SIZE; //0-63

    uint base_addr_x = ( (BLOCK_DIM*block_x) //
                       + FREQUENCY_BAND*NUM_ELEMENTS
                       + DATA_SET_INDEX*NUM_ELEMENTS*NUM_FREQUENCIES )*2u; //times 2 because there are pairs of numbers for the complex values
    uint base_addr_y = ( (BLOCK_DIM*block_y)
                       + FREQUENCY_BAND*NUM_ELEMENTS
                       + DATA_SET_INDEX*NUM_ELEMENTS*NUM_FREQUENCIES )*2u;

    uint8 xVals;
    uint8 yVals; //using vectors since data will be in contiguous memory spaces and can then load 8 items at a time (in the hopes of getting coalesced loads
    int  xValPairs[8u];
    int  yValPairs[8u];

    //synchronize then load
    barrier(CLK_LOCAL_MEM_FENCE);
    //want to load 32 complex values (i.e. 64 values)
    localDataX[local_index] = dataIn[base_addr_x+local_index]; //local_index has 64 contiguous entries
    localDataY[local_index] = dataIn[base_addr_y+local_index];
    barrier(CLK_LOCAL_MEM_FENCE);

    //load relevant values for this work item
    xVals = vload8(LOCAL_X,localDataX); //offsets are in sizes of the vector, so 8 uints big
    yVals = vload8(LOCAL_Y,localDataY);

    //if registers are the slowing point of this algorithm, then can possibly reuse variables, though this should be okay as a test
    xValPairs[0u] = xVals.s0 + xVals.s1; //sum of real and imaginary
    xValPairs[1u] = xVals.s0 - xVals.s1; //difference of real and imaginary
    xValPairs[2u] = xVals.s2 + xVals.s3;
    xValPairs[3u] = xVals.s2 - xVals.s3;
    xValPairs[4u] = xVals.s4 + xVals.s5;
    xValPairs[5u] = xVals.s4 - xVals.s5;
    xValPairs[6u] = xVals.s6 + xVals.s7;
    xValPairs[7u] = xVals.s6 - xVals.s7;

    yValPairs[0u] = yVals.s0 + yVals.s1;
    yValPairs[1u] = yVals.s1 - yVals.s0; //note the index swap to perform the extra subtraction
    yValPairs[2u] = yVals.s2 + yVals.s3;
    yValPairs[3u] = yVals.s3 - yVals.s2;
    yValPairs[4u] = yVals.s4 + yVals.s5;
    yValPairs[5u] = yVals.s5 - yVals.s4;
    yValPairs[6u] = yVals.s6 + yVals.s7;
    yValPairs[7u] = yVals.s7 - yVals.s6;

    //output results
    //Each work item outputs 4 x 4 complex values (so 32 values rather than 16)
    //
    //offset to the next row is 8 (local_x vals) x 8 vals = 64
    //each y takes care of 4 values, so y * 4*64
    //
    //16 pairs * 8 (local_size(0)) * 8 (local_size(1)) = 1024
    uint addr_o = (DATA_SET_INDEX * NUM_FREQUENCIES * NUM_BLOCKS_x_2048)
                    + (FREQUENCY_BAND * NUM_BLOCKS_x_2048)
                    + ((BLOCK_ID * 2048u)
                    + (LOCAL_Y * 256u) + (LOCAL_X * 8u));
    int num_timesamples_x_128 = (counters[DATA_SET_INDEX*NUM_FREQUENCIES+FREQUENCY_BAND])*128*256;//<<15;
    // test of coverage
    //row 0
//     corr_buf[addr_o+0u]= num_timesamples_x_128;// 1;// num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[0u]); //real value correction //num_timesamples_x_128;//counters[DATA_SET_INDEX]*256;//
//     corr_buf[addr_o+1u]= num_timesamples_x_128;// 1;//                         8u*(xValPairs[1u]+yValPairs[1u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+2u]= num_timesamples_x_128;// 1;// num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[0u]); //note that x changes, but y stays the same //num_timesamples_x_128;//
//     corr_buf[addr_o+3u]= num_timesamples_x_128;// 1;//                         8u*(xValPairs[3u]+yValPairs[1u]);
//     corr_buf[addr_o+4u]= num_timesamples_x_128;// 1;// num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[0u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+5u]= num_timesamples_x_128;// 1;//                         8u*(xValPairs[5u]+yValPairs[1u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+6u]= num_timesamples_x_128;// 1;// num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[0u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+7u]= num_timesamples_x_128;// 1;//                         8u*(xValPairs[7u]+yValPairs[1u]);
//     //row 1
//     corr_buf[addr_o+64u]= num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[2u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+65u]= num_timesamples_x_128;//1;//                         8u*(xValPairs[1u]+yValPairs[3u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+66u]= num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[2u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+67u]= num_timesamples_x_128;//1;//                         8u*(xValPairs[3u]+yValPairs[3u]);
//     corr_buf[addr_o+68u]= num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[2u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+69u]= num_timesamples_x_128;//1;//                         8u*(xValPairs[5u]+yValPairs[3u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+70u]= num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[2u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+71u]= num_timesamples_x_128;//1;//                         8u*(xValPairs[7u]+yValPairs[3u]);
//     //row 2
//     corr_buf[addr_o+128u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[4u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+129u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[1u]+yValPairs[5u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+130u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[4u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+131u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[3u]+yValPairs[5u]);
//     corr_buf[addr_o+132u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[4u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+133u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[5u]+yValPairs[5u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+134u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[4u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+135u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[7u]+yValPairs[5u]);
//     //row 3
//     corr_buf[addr_o+192u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[6u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+193u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[1u]+yValPairs[7u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+194u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[6u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+195u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[3u]+yValPairs[7u]);
//     corr_buf[addr_o+196u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[6u]); //real value correction //num_timesamples_x_128;//
//     corr_buf[addr_o+197u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[5u]+yValPairs[7u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
//     corr_buf[addr_o+198u]=num_timesamples_x_128;//1;// num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[6u]); //num_timesamples_x_128;//
//     corr_buf[addr_o+199u]=num_timesamples_x_128;//1;//                         8u*(xValPairs[7u]+yValPairs[7u]);

    //row 0
    corr_buf[addr_o+0u]=   num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[0u]); //real value correction //num_timesamples_x_128;//counters[DATA_SET_INDEX]*256;//
    corr_buf[addr_o+1u]=                           8u*(xValPairs[1u]+yValPairs[1u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+2u]=   num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[0u]); //note that x changes, but y stays the same //num_timesamples_x_128;//
    corr_buf[addr_o+3u]=                           8u*(xValPairs[3u]+yValPairs[1u]);
    corr_buf[addr_o+4u]=   num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[0u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+5u]=                           8u*(xValPairs[5u]+yValPairs[1u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+6u]=   num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[0u]); //num_timesamples_x_128;//
    corr_buf[addr_o+7u]=                           8u*(xValPairs[7u]+yValPairs[1u]);
    //row 1
    corr_buf[addr_o+64u]=  num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[2u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+65u]=                          8u*(xValPairs[1u]+yValPairs[3u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+66u]=  num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[2u]); //num_timesamples_x_128;//
    corr_buf[addr_o+67u]=                          8u*(xValPairs[3u]+yValPairs[3u]);
    corr_buf[addr_o+68u]=  num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[2u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+69u]=                          8u*(xValPairs[5u]+yValPairs[3u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+70u]=  num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[2u]); //num_timesamples_x_128;//
    corr_buf[addr_o+71u]=                          8u*(xValPairs[7u]+yValPairs[3u]);
    //row 2
    corr_buf[addr_o+128u]= num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[4u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+129u]=                         8u*(xValPairs[1u]+yValPairs[5u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+130u]= num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[4u]); //num_timesamples_x_128;//
    corr_buf[addr_o+131u]=                         8u*(xValPairs[3u]+yValPairs[5u]);
    corr_buf[addr_o+132u]= num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[4u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+133u]=                         8u*(xValPairs[5u]+yValPairs[5u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+134u]= num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[4u]); //num_timesamples_x_128;//
    corr_buf[addr_o+135u]=                         8u*(xValPairs[7u]+yValPairs[5u]);
    //row 3
    corr_buf[addr_o+192u]= num_timesamples_x_128 - 8u*(xValPairs[0u]+yValPairs[6u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+193u]=                         8u*(xValPairs[1u]+yValPairs[7u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+194u]= num_timesamples_x_128 - 8u*(xValPairs[2u]+yValPairs[6u]); //num_timesamples_x_128;//
    corr_buf[addr_o+195u]=                         8u*(xValPairs[3u]+yValPairs[7u]);
    corr_buf[addr_o+196u]= num_timesamples_x_128 - 8u*(xValPairs[4u]+yValPairs[6u]); //real value correction //num_timesamples_x_128;//
    corr_buf[addr_o+197u]=                         8u*(xValPairs[5u]+yValPairs[7u]); //imaginary value correction (the extra subtraction in the notes has been performed by swapping order above
    corr_buf[addr_o+198u]= num_timesamples_x_128 - 8u*(xValPairs[6u]+yValPairs[6u]); //num_timesamples_x_128;//
    corr_buf[addr_o+199u]=                         8u*(xValPairs[7u]+yValPairs[7u]);

}
