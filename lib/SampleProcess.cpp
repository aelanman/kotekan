#include "SampleProcess.h"
#include "errors.h"

SampleProcess::SampleProcess(struct Config &config) :
    KotekanProcess(config, std::bind(&SampleProcess::main_thread, this)) {
}

SampleProcess::~SampleProcess() {
}

void SampleProcess::main_thread() {
    INFO("Sample Process, reached main_thread!");
    while (!stop_thread) {
        INFO("In thread!");
    }
}

