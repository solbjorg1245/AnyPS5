#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WORKERSAMPLER_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_WORKERSAMPLER_HPP

namespace AgcDriver {

// Starts sampling the calling thread when APS5_SAMPLE_WORKER names an output file.
void StartWorkerSampler();
void StopWorkerSampler();

// APS5_LABEL_CENSUS=1 (P-LABEL; default off): a thread census on the 10 s digest, each thread's CPU
// time over the last 10 s (the [threads] line, started by StartWorkerSampler). NameCensusThread
// names the calling driver thread (queue workers, the recording pipe, the presenter) for it.
void NameCensusThread(const char* role);

}

#endif
