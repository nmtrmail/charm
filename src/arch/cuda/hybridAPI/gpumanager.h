#ifndef __GPUMANAGER_H_
#define __GPUMANAGER_H_

#include <cuda_runtime.h>
#include <vector>
#include <unordered_map>

#include "converse.h"
#include "hapi.h"
#include "hapi_impl.h"
#include "devicemanager.h"

// Initial size of the user-addressed portion of host/device buffer arrays;
// the system-addressed portion of host/device buffer arrays (used when there
// is no need to share buffers between work requests) will be equivalant in size.
// FIXME hard-coded maximum
#if CMK_SMP
#define NUM_BUFFERS 4096
#else
#define NUM_BUFFERS 256
#endif

// Shared memory information for CUDA IPC
struct cuda_ipc_shm_info {
  volatile int sync_flag;
  cudaIpcEventHandle_t event_handle;
  cudaIpcMemHandle_t mem_handle;
};

// Per-process struct containing local data for CUDA IPC
struct cuda_ipc_local_info {
  cudaEvent_t event;
  void* buffer;
};

// Contains per-process data and methods needed by HAPI.
struct GPUManager {
  std::vector<BufferPool> mempool_free_bufs_;
  std::vector<size_t> mempool_boundaries_;
  bool mempool_initialized_;

  // The runtime system keeps track of all allocated buffers on the GPU.
  // The following arrays contain pointers to host (CPU) data and the
  // corresponding data on the device (GPU).
  void **host_buffers_;
  void **device_buffers_;

  // Used to assign buffer IDs automatically by the system if the user
  // specifies an invalid buffer ID.
  int next_buffer_;

  cudaStream_t *streams_;
  int n_streams_;
  int last_stream_id_;

#ifdef HAPI_CUDA_CALLBACK
  int host_to_device_cb_idx_;
  int kernel_cb_idx_;
  int device_to_host_cb_idx_;
  int light_cb_idx_; // for lightweight version
#endif

  int running_kernel_idx_;
  int data_setup_idx_;
  int data_cleanup_idx_;

#ifdef HAPI_TRACE
  gpuEventTimer gpu_events_[QUEUE_SIZE_INIT * 3];
  std::atomic<int> time_idx_;
#endif

#ifdef HAPI_INSTRUMENT_WRS
  std::vector<std::vector<std::vector<hapiRequestTimeInfo>>> avg_times_;
  bool init_instr_;
#endif

#if CMK_SMP
  CmiNodeLock queue_lock_;
  CmiNodeLock progress_lock_;
  CmiNodeLock stream_lock_;
  CmiNodeLock mempool_lock_;
  CmiNodeLock inst_lock_;
  CmiNodeLock device_mapping_lock;
#endif

#ifdef HAPI_CUDA_CALLBACK
#endif

  // GPU devices accessible to this process
  int device_count;
  int device_count_on_physical_node;
  DeviceManager *device_managers;
  std::unordered_map<int, DeviceManager*> device_map;

  // Eager communication buffer
  bool use_eager_comm_buffer;
  size_t eager_comm_buffer_size;

  // POSIX shared memory for sharing CUDA IPC handles between processes on the same host
  void* shm_ptr;
  char* shm_name;
  int shm_file;
  size_t shm_size;
  int shm_my_index;

  // CUDA IPC handles opened for processes on the same node
  cuda_ipc_local_info* cuda_ipc_local_infos;

  void init();
  int createStreams();
  int createNStreams(int);
  void destroyStreams();
  cudaStream_t getNextStream();
  cudaStream_t getStream(int);
  int getNStreams();
  void allocateBuffers(hapiWorkRequest*);
  void hostToDeviceTransfer(hapiWorkRequest*);
  void deviceToHostTransfer(hapiWorkRequest*);
  void freeBuffers(hapiWorkRequest*);
  void runKernel(hapiWorkRequest*);

};

void GPUManager::init() {
  next_buffer_ = NUM_BUFFERS;
  streams_ = NULL;
  n_streams_ = 0;
  last_stream_id_ = -1;
  running_kernel_idx_ = 0;
  data_setup_idx_ = 0;
  data_cleanup_idx_ = 0;

#if CMK_SMP
  // create mutex locks
  queue_lock_ = CmiCreateLock();
  progress_lock_ = CmiCreateLock();
  stream_lock_ = CmiCreateLock();
  mempool_lock_ = CmiCreateLock();
  inst_lock_ = CmiCreateLock();
  device_mapping_lock = CmiCreateLock();
#endif

#ifdef HAPI_TRACE
  time_idx_ = 0;
#endif

  // How many GPUs are available to this process?
  hapiCheck(cudaGetDeviceCount(&device_count));

  // If there are more devices than threads, reduce the number of devices
  if (device_count > CmiNodeSize(CmiMyNode())) {
    device_count = CmiNodeSize(CmiMyNode());
  }

  // FIXME: Assumes GPU count is the same across all processes
  device_count_on_physical_node = device_count * (CmiNumNodes() / CmiNumPhysicalNodes());

  // Create a DeviceManager for each GPU
  device_managers = new DeviceManager[device_count];

  // Eager communication buffer
  use_eager_comm_buffer = true;
  eager_comm_buffer_size = 1 << 26; // 64MB by default

  // Shared memory region for CUDA IPC
  shm_ptr = NULL;
  shm_name = NULL;
  shm_file = -1;
  shm_size = 0;
  shm_my_index = -1;

  // Local information on opened CUDA IPC event and buffer
  cuda_ipc_local_infos = NULL;

  // allocate host/device buffers array (both user and system-addressed)
  host_buffers_ = new void*[NUM_BUFFERS*2];
  device_buffers_ = new void*[NUM_BUFFERS*2];

  // initialize device array to NULL
  for (int i = 0; i < NUM_BUFFERS*2; i++) {
    device_buffers_[i] = NULL;
  }

#ifdef HAPI_TRACE
  traceRegisterUserEvent("GPU Memory Setup", GpuMemSetup);
  traceRegisterUserEvent("GPU Kernel Execution", GpuKernelExec);
  traceRegisterUserEvent("GPU Memory Cleanup", GpuMemCleanup);
#endif

  // set up mempool metadata
  mempool_initialized_ = false;
  mempool_boundaries_.resize(HAPI_MEMPOOL_NUM_SLOTS);

  size_t buf_size = HAPI_MEMPOOL_MIN_BUFFER_SIZE;
  for(int i = 0; i < HAPI_MEMPOOL_NUM_SLOTS; i++){
    mempool_boundaries_[i] = buf_size;
    buf_size = buf_size << 1;
  }

#ifdef HAPI_INSTRUMENT_WRS
  init_instr_ = false;
#endif
}

// Creates streams equal to the maximum number of concurrent kernels,
// which depends on the compute capability of the device.
// Returns the number of created streams.
int GPUManager::createStreams() {
  int device;
  cudaDeviceProp device_prop;
  hapiCheck(cudaGetDevice(&device));
  hapiCheck(cudaGetDeviceProperties(&device_prop, device));

  int new_n_streams = 0;

  if (device_prop.major == 3) {
    if (device_prop.minor == 0)
      new_n_streams = 16;
    else if (device_prop.minor == 2)
      new_n_streams = 4;
    else // 3.5, 3.7 or unknown 3.x
      new_n_streams = 32;
  }
  else if (device_prop.major == 5) {
    if (device_prop.minor == 3)
      new_n_streams = 16;
    else // 5.0, 5.2 or unknown 5.x
      new_n_streams = 32;
  }
  else if (device_prop.major == 6) {
    if (device_prop.minor == 1)
      new_n_streams = 32;
    else if (device_prop.minor == 2)
      new_n_streams = 16;
    else // 6.0 or unknown 6.x
      new_n_streams = 128;
  }
  else // unknown (future) compute capability
    new_n_streams = 128;
#if !CMK_SMP
  // Allocate total physical streams between GPU managers sharing a device...
  // i.e. PEs / num devices
  int device_count;
  hapiCheck(cudaGetDeviceCount(&device_count));
  int pes_per_device = CmiNumPesOnPhysicalNode(0) / device_count;
  pes_per_device = pes_per_device > 0 ? pes_per_device : 1;
  new_n_streams =  (new_n_streams + pes_per_device - 1) / pes_per_device;
#endif

  int total_n_streams = createNStreams(new_n_streams);

  return total_n_streams;
}

int GPUManager::createNStreams(int new_n_streams) {
  if (new_n_streams <= n_streams_) {
    return n_streams_;
  }

  cudaStream_t* old_streams = streams_;

  streams_ = new cudaStream_t[new_n_streams];

  int i = 0;
  // Copy old streams
  for (; i < n_streams_; i++) {
    // TODO alt. use memcpy?
    streams_[i] = old_streams[i];
  }

  // Create new streams
  for (; i < new_n_streams; i++) {
    hapiCheck(cudaStreamCreate(&streams_[i]));
  }

  // Update
  n_streams_ = new_n_streams;
  delete [] old_streams;

  return n_streams_;
}

void GPUManager::destroyStreams() {
  if (streams_) {
    for (int i = 0; i < n_streams_; i++) {
      hapiCheck(cudaStreamDestroy(streams_[i]));
    }
  }
}

cudaStream_t GPUManager::getNextStream() {
  if (streams_ == NULL)
    return NULL;

  last_stream_id_ = (++last_stream_id_) % n_streams_;
  return streams_[last_stream_id_];
}

cudaStream_t GPUManager::getStream(int i) {
  if (streams_ == NULL)
    return NULL;

  if (i < 0 || i >= n_streams_)
    CmiAbort("[HAPI] invalid stream ID");
  return streams_[i];
}

int GPUManager::getNStreams() {
  if (!streams_) // NULL - default stream
    return 1;

  return n_streams_;
}

// Allocates device buffers.
void GPUManager::allocateBuffers(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;

    // if index value is invalid, use an available ID
    if (index < 0 || index >= NUM_BUFFERS) {
      bool is_found = false;
      for (int j = next_buffer_; j < NUM_BUFFERS*2; j++) {
        if (device_buffers_[j] == NULL) {
          index = j;
          is_found = true;
          break;
        }
      }

      // if no index was found, try to search for a value at the
      // beginning of the system addressed space
      if (!is_found) {
        for (int j = NUM_BUFFERS; j < next_buffer_; j++) {
          if (device_buffers_[j] == NULL) {
            index = j;
            is_found = true;
            break;
          }
        }
      }

      if (!is_found) {
        CmiAbort("[HAPI] ran out of device buffer indices");
      }

      next_buffer_ = index + 1;
      if (next_buffer_ == NUM_BUFFERS*2) {
        next_buffer_ = NUM_BUFFERS;
      }

      bi.id = index;
    }

    if (device_buffers_[index] == NULL) {
      // allocate device memory
      hapiCheck(cudaMalloc((void **)&device_buffers_[index], size));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] allocated buffer %d at %p, time: %.2f, size: %zu\n",
             index, device_buffers_[index], cutGetTimerValue(timerHandle),
             size);
#endif
    }
  }
}

// Initiates host-to-device data transfer.
void GPUManager::hostToDeviceTransfer(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;
    host_buffers_[index] = bi.host_buffer;

    if (bi.transfer_to_device) {
      hapiCheck(cudaMemcpyAsync(device_buffers_[index], host_buffers_[index], size,
                                cudaMemcpyHostToDevice, wr->stream));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] transferring buffer %d from host to device, time: %.2f, "
             "size: %zu\n", index, cutGetTimerValue(timerHandle), size);
#endif
    }
  }
}

// Initiates device-to-host data transfer.
void GPUManager::deviceToHostTransfer(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;

    if (bi.transfer_to_host) {
      hapiCheck(cudaMemcpyAsync(host_buffers_[index], device_buffers_[index], size,
                                cudaMemcpyDeviceToHost, wr->stream));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] transferring buffer %d from device to host, time %.2f, "
             "size: %zu\n", index, cutGetTimerValue(timerHandle), size);
#endif
    }
  }
}

// Frees device buffers.
void GPUManager::freeBuffers(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;

    if (bi.need_free) {
      hapiCheck(cudaFree(device_buffers_[index]));
      device_buffers_[index] = NULL;

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] freed buffer %d, time %.2f\n",
             index, cutGetTimerValue(timerHandle));
#endif
    }
  }
}

// Run the user's kernel for the given work request.
// This used to be a switch statement defined by the user to allow the runtime
// to execute the correct kernel.
void GPUManager::runKernel(hapiWorkRequest* wr) {
	if (wr->runKernel) {
		wr->runKernel(wr, wr->stream, device_buffers_);
	}
	// else, might be only for data transfer (or might be a bug?)
}

#endif // __GPUMANAGER_H_
