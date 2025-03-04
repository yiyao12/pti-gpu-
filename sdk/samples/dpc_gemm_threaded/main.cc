//==============================================================
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT
// =============================================================

//
// Based on dpc_gemm sample. Added multithreading
//

#include <string.h>

#include <CL/sycl.hpp>
#include <cstdlib>
#include <memory>
#include <thread>
#include <stdarg.h>

#include "pti_view.h"
#include "utils.h"
#include "samples_utils.h"

#define A_VALUE 0.128f
#define B_VALUE 0.256f
#define MAX_EPS 1.0e-4f

static float Check(const std::vector<float>& a, float value) {
  assert(value > MAX_EPS);

  float eps = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    eps += fabs((a[i] - value) / value);
  }

  return eps / a.size();
}

void GEMM(const float* a, const float* b, float* c, unsigned size,
          sycl::id<2> id) {
  int i = id.get(0);
  int j = id.get(1);
  float sum = 0.0f;
  for (unsigned k = 0; k < size; ++k) {
    sum += a[i * size + k] * b[k * size + j];
  }
  c[i * size + j] = sum;
}

static float RunAndCheck(sycl::queue queue, const std::vector<float>& a,
                         const std::vector<float>& b, std::vector<float>& c,
                         unsigned size, float expected_result) {
  assert(size > 0);
  assert(a.size() == size * size);
  assert(b.size() == size * size);
  assert(c.size() == size * size);

  double time = 0.0;

  try {
    sycl::buffer<float, 1> a_buf(a.data(), a.size());
    sycl::buffer<float, 1> b_buf(b.data(), b.size());
    sycl::buffer<float, 1> c_buf(c.data(), c.size());

    sycl::event event = queue.submit([&](sycl::handler& cgh) {
      auto a_acc = a_buf.get_access<sycl::access::mode::read>(cgh);
      auto b_acc = b_buf.get_access<sycl::access::mode::read>(cgh);
      auto c_acc = c_buf.get_access<sycl::access::mode::write>(cgh);

      cgh.parallel_for<class __GEMM>(
          sycl::range<2>(size, size), [=](sycl::id<2> id) {
            auto a_acc_ptr = a_acc.get_multi_ptr<sycl::access::decorated::no>();
            auto b_acc_ptr = b_acc.get_multi_ptr<sycl::access::decorated::no>();
            auto c_acc_ptr = c_acc.get_multi_ptr<sycl::access::decorated::no>();
            GEMM(a_acc_ptr.get(), b_acc_ptr.get(), c_acc_ptr.get(), size, id);
          });
    });
    queue.wait_and_throw();

    auto start =
        event.get_profiling_info<sycl::info::event_profiling::command_start>();
    auto end =
        event.get_profiling_info<sycl::info::event_profiling::command_end>();
    time = static_cast<double>(end - start) / NSEC_IN_SEC;
  } catch (const sycl::exception& e) {
    std::cout << "[ERROR] " << e.what() << std::endl;
    throw;
  }

  std::cout << "\tMatrix multiplication time: " << time << " sec" << std::endl;

  return Check(c, expected_result);
}

static void Compute(sycl::queue queue, const std::vector<float>& a,
                    const std::vector<float>& b, std::vector<float>& c,
                    unsigned size, unsigned repeat_count,
                    float expected_result) {
  for (unsigned i = 0; i < repeat_count; ++i) {
    float eps = RunAndCheck(queue, a, b, c, size, expected_result);
    std::cout << "Results are " << ((eps < MAX_EPS) ? "" : "IN")
              << "CORRECT with accuracy: " << eps << std::endl;
  }
}

void StartTracing() {
  ptiViewEnable(PTI_VIEW_DEVICE_GPU_KERNEL);
  ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_COPY);
  ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_FILL);
  ptiViewEnable(PTI_VIEW_SYCL_RUNTIME_CALLS);
  ptiViewEnable(PTI_VIEW_COLLECTION_OVERHEAD);
}

void StopTracing() {
  ptiViewDisable(PTI_VIEW_DEVICE_GPU_KERNEL);
  ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_COPY);
  ptiViewDisable(PTI_VIEW_DEVICE_GPU_MEM_FILL);
  ptiViewDisable(PTI_VIEW_SYCL_RUNTIME_CALLS);
  ptiViewDisable(PTI_VIEW_COLLECTION_OVERHEAD);
}

const unsigned max_thread_count = 64;
const unsigned max_size = 8192;
const unsigned min_size = 32;

void Usage(const char* name) {

  std::cout << " Calculating floating point matrix multiply on gpu, submitting the work from many CPU threads\n";
  std::cout << name << " [ [number of threads, default=2, max=" << max_thread_count
            << "],  [matrix size, default=1024, max=" << max_size << "], [repetition count, default=4]] \n";
}

int main(int argc, char* argv[]) {

  int exit_code = EXIT_SUCCESS;
  unsigned thread_count = 2;
  unsigned repeat_count = 4;
  unsigned size = 1024;

  if (argc == 2 &&
      ( strcmp(argv[1], "-?") == 0 or strcmp(argv[1], "-h") == 0  or strcmp(argv[1], "--help" ) == 0) ){
    Usage(argv[0]);
    return EXIT_SUCCESS;
  }

  try {
    unsigned temp;
    if (argc > 1) {
      temp = std::stoul(argv[1]);
      thread_count = (temp < 1) ? 1 :
                    (temp > max_thread_count) ?  max_thread_count : temp;
    }
    if (argc > 2) {
      temp = std::stoul(argv[2]);
      size = (temp < min_size) ? min_size :
                    (temp > max_size) ?  max_size : temp;
    }

    if (argc > 3) {
      temp = std::stoul(argv[3]);
      repeat_count = (temp < 1) ? 1 : temp;
    }
  }

  catch(...) {
    Usage(argv[0]);
    return EXIT_FAILURE;
  }

  ptiViewEnable(PTI_VIEW_DEVICE_GPU_KERNEL);
  ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_COPY);
  ptiViewEnable(PTI_VIEW_DEVICE_GPU_MEM_FILL);

  ptiViewSetCallbacks(
      [](auto** buf, auto* buf_size) {
        *buf_size = 1000*sizeof(pti_view_record_kernel);
        void* ptr = ::operator new(*buf_size);
        ptr = std::align(8, sizeof(unsigned char), ptr, *buf_size);
        *buf = reinterpret_cast<unsigned char*>(ptr);
        if (!*buf) {
          std::abort();
        }
        return;
      },
      [](auto* buf, auto buf_size, auto valid_buf_size) {
        if (!buf || !valid_buf_size || !buf_size) {
          std::cerr << "Received empty buffer" << '\n';
          if (valid_buf_size) {
            ::operator delete(buf);
          }
          return;
        }
        pti_view_record_base* ptr = nullptr;

        auto validate_timestamps = [](uint64_t count, ...) {
          va_list args;
          va_start(args, count);
          if (1LU == count) return;
          uint64_t prev_stamp = va_arg(args, uint64_t);
          uint64_t next_stamp = 0LU;
          for (uint64_t i = 1; i < count; ++i)
          {
            next_stamp = va_arg(args, uint64_t);
            assert(prev_stamp<=next_stamp);
            prev_stamp = next_stamp;
          }
          va_end(args);
          return;

        };
        while (true) {
          auto buf_status =
              ptiViewGetNextRecord(buf, valid_buf_size, &ptr);
          if (buf_status == pti_result::PTI_STATUS_END_OF_BUFFER) {
            std::cout << "Reached End of buffer" << '\n';
            break;
          }
          if (buf_status != pti_result::PTI_SUCCESS) {
            std::cerr << "Found Error Parsing Records from PTI" << '\n';
            break;
          }
          switch (ptr->_view_kind) {
            case pti_view_kind::PTI_VIEW_INVALID: {
              std::cout << "Found Invalid Record" << '\n';
              break;
            }
            case pti_view_kind::PTI_VIEW_SYCL_RUNTIME_CALLS: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              std::cout << "Found Sycl Runtime Record" << '\n';
              samples_utils::dump_record(reinterpret_cast<pti_view_record_sycl_runtime *>(ptr));
              break;
            }
            case pti_view_kind::PTI_VIEW_COLLECTION_OVERHEAD: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              samples_utils::dump_record(reinterpret_cast<pti_view_record_overhead *>(ptr));
              break;
            }
            case pti_view_kind::PTI_VIEW_EXTERNAL_CORRELATION: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              samples_utils::dump_record(reinterpret_cast<pti_view_record_external_correlation *>(ptr));
              break;
            }
            case pti_view_kind:: PTI_VIEW_DEVICE_GPU_MEM_COPY: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              std::cout << "Found Memory Record" << '\n';

              pti_view_record_memory_copy* p_memory_rec = reinterpret_cast<pti_view_record_memory_copy*>(ptr);
              samples_utils::dump_record(p_memory_rec);
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              validate_timestamps(4, p_memory_rec->_append_timestamp, p_memory_rec->_submit_timestamp,
                                     p_memory_rec->_start_timestamp, p_memory_rec->_end_timestamp);
              break;
            }
            case pti_view_kind:: PTI_VIEW_DEVICE_GPU_MEM_FILL: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              std::cout << "Found Memory Record" << '\n';

              pti_view_record_memory_fill* p_memory_rec = reinterpret_cast<pti_view_record_memory_fill*>(ptr);
              samples_utils::dump_record(p_memory_rec);
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              validate_timestamps(4, p_memory_rec->_append_timestamp, p_memory_rec->_submit_timestamp,
                                     p_memory_rec->_start_timestamp, p_memory_rec->_end_timestamp);
              break;
            }
            case pti_view_kind::PTI_VIEW_DEVICE_GPU_KERNEL: {
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              std::cout << "Found Kernel Record" << '\n';

              pti_view_record_kernel* p_kernel_rec = reinterpret_cast<pti_view_record_kernel*>(ptr);
              samples_utils::dump_record(p_kernel_rec);
              std::cout << "---------------------------------------------------"
                           "-----------------------------"
                        << '\n';
              validate_timestamps(6, p_kernel_rec->_sycl_task_begin_timestamp, p_kernel_rec->_sycl_enqk_begin_timestamp,
                                     p_kernel_rec->_append_timestamp, p_kernel_rec->_submit_timestamp,
                                     p_kernel_rec->_start_timestamp, p_kernel_rec->_end_timestamp);
              break;
            }
            default: {
              std::cerr << "This shouldn't happen" << '\n';
              break;
            }
          }
        }
        ::operator delete(buf);
      });
  StartTracing();
  sycl::device dev;
  try {
    dev = sycl::device(sycl::gpu_selector_v);
    sycl::property_list prop_list{sycl::property::queue::enable_profiling()};
    sycl::queue queue(dev, sycl::async_handler{}, prop_list);
    if (argc > 1 && strcmp(argv[1], "cpu") == 0) {
      dev = sycl::device(sycl::cpu_selector_v);
      std::cerr << "PTI doesn't support cpu profiling yet" << '\n';
      std::exit(EXIT_FAILURE);
    } else if (argc > 1 && strcmp(argv[1], "host") == 0) {
      dev = sycl::device(sycl::default_selector_v);
      std::cerr << "PTI doesn't support host profiling yet" << '\n';
      std::exit(EXIT_FAILURE);
    }

    float expected_result = A_VALUE * B_VALUE * size;

    auto threadFunction = [&queue] (unsigned _size, unsigned _repeat_count, float _expected_result) {

      std::vector<float> a(_size * _size, A_VALUE);
      std::vector<float> b(_size * _size, B_VALUE);
      std::vector<float> c(_size * _size, 0.0f);

      auto start = std::chrono::steady_clock::now();
      Compute(queue, a, b, c, _size, _repeat_count, _expected_result);
      auto end = std::chrono::steady_clock::now();
      std::chrono::duration<float> time = end - start;

      std::cout << "\t-- Total execution time: " << time.count() << " sec" << std::endl;
    };

    std::cout << "DPC++ Matrix Multiplication (CPU threads: " << thread_count << ", matrix size: " << size << " x "
              << size << ", repeats: " << repeat_count << " times)" << std::endl;
    std::cout << "Target device: "
              << queue.get_info<sycl::info::queue::device>()
                    .get_info<sycl::info::device::name>()
              << std::endl;

    std::vector<std::thread> the_threads;
    for (unsigned i=0; i<thread_count; i++) {
      std::thread t = std::thread( threadFunction, size, repeat_count, expected_result);
      the_threads.push_back(std::move(t));
    }

    for(auto& th : the_threads ) {
      if (th.joinable()) {
      th.join();
      }
    }
  } catch (const sycl::exception &e) {
    std::cerr << "Error: Exception while executing SYCL " << e.what() << '\n';
    std::cerr << "\tError code: " << e.code().value()
              << "\n\tCategory: " << e.category().name()
              << "\n\tMessage: " << e.code().message() << '\n';
    exit_code = EXIT_FAILURE;
  } catch (const std::exception &e) {
    std::cerr << "Error: Exception caught " << e.what() << '\n';
    exit_code = EXIT_FAILURE;
  } catch (...) {
    std::cerr << "Error: Unknown exception caught." << '\n';
    exit_code = EXIT_FAILURE;
  }

  StopTracing();
  auto flush_results = ptiFlushAllViews();
  return exit_code;
}
