// Copyright (c) 2020 - present Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <boost/scope_exit.hpp>
#include <gtest/gtest.h>
#include <math.h>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../client_utils.h"
#include "accuracy_test.h"
#include "fftw_transform.h"
#include "gpubuf.h"
#include "rocfft.h"
#include "rocfft_against_fftw.h"

// Given an array type, return the name as a string.
std::string array_type_name(const rocfft_array_type type)
{
    switch(type)
    {
    case rocfft_array_type_complex_interleaved:
        return "rocfft_array_type_complex_interleaved";
    case rocfft_array_type_complex_planar:
        return "rocfft_array_type_complex_planar";
    case rocfft_array_type_real:
        return "rocfft_array_type_real";
    case rocfft_array_type_hermitian_interleaved:
        return "rocfft_array_type_hermitian_interleaved";
    case rocfft_array_type_hermitian_planar:
        return "rocfft_array_type_hermitian_planar";
    case rocfft_array_type_unset:
        return "rocfft_array_type_unset";
    }
    return "";
}

// Function to return a string with the gpu parameters.
std::string gpu_params(const std::vector<size_t>&    gpu_ilength_cm,
                       const std::vector<size_t>&    gpu_istride_cm,
                       const size_t                  gpu_idist,
                       const std::vector<size_t>&    gpu_ostride_cm,
                       const size_t                  gpu_odist,
                       const size_t                  nbatch,
                       const rocfft_precision        precision,
                       const rocfft_result_placement place,
                       const rocfft_array_type       itype,
                       const rocfft_array_type       otype)
{
    std::stringstream ss;
    ss << "\nGPU params:\n";
    ss << "\tgpu_ilength_cm:";
    for(auto i : gpu_ilength_cm)
        ss << " " << i;
    ss << "\n";
    ss << "\tgpu_istride_cm:";
    for(auto i : gpu_istride_cm)
        ss << " " << i;
    ss << "\n";
    ss << "\tgpu_idist: " << gpu_idist << "\n";

    ss << "\tgpu_ostride_cm:";
    for(auto i : gpu_ostride_cm)
        ss << " " << i;
    ss << "\n";
    ss << "\tgpu_odist: " << gpu_odist << "\n";

    ss << "\tbatch: " << nbatch << "\n";

    if(place == rocfft_placement_inplace)
        ss << "\tin-place\n";
    else
        ss << "\tout-of-place\n";
    ss << "\t" << array_type_name(itype) << " -> " << array_type_name(otype) << "\n";
    if(precision == rocfft_precision_single)
        ss << "\tsingle-precision\n";
    else
        ss << "\tdouble-precision\n";
    return ss.str();
}

// Compute a FFT using rocFFT and compare with the provided CPU reference computation.
void rocfft_transform(const std::vector<size_t>&            length,
                      const std::vector<size_t>&            istride,
                      const std::vector<size_t>&            ostride,
                      const size_t                          nbatch,
                      const rocfft_precision                precision,
                      const rocfft_transform_type           transformType,
                      const rocfft_array_type               itype,
                      const rocfft_array_type               otype,
                      const rocfft_result_placement         place,
                      const std::vector<size_t>&            cpu_istride,
                      const std::vector<size_t>&            cpu_ostride,
                      const size_t                          cpu_idist,
                      const size_t                          cpu_odist,
                      const rocfft_array_type               cpu_itype,
                      const rocfft_array_type               cpu_otype,
                      const std::shared_future<fftw_data_t> cpu_input,
                      const std::shared_future<fftw_data_t> cpu_output,
                      const size_t                          ramgb,
                      const std::shared_future<VectorNorms> cpu_output_norm)
{
    if(ramgb > 0)
    {
        // Estimate the amount of memory needed, and skip if it's more than we allow.

        // Host input, output, and input copy: 3 buffers, all contiguous.
        size_t needed_ram
            = 3 * std::accumulate(length.begin(), length.end(), 1, std::multiplies<size_t>());

        // GPU input buffer:
        needed_ram += std::inner_product(length.begin(), length.end(), istride.begin(), 0);

        // GPU output buffer:
        needed_ram += std::inner_product(length.begin(), length.end(), ostride.begin(), 0);

        // Account for precision and data type:
        if(transformType != rocfft_transform_type_real_forward
           || transformType != rocfft_transform_type_real_inverse)
        {
            needed_ram *= 2;
        }
        switch(precision)
        {
        case rocfft_precision_single:
            needed_ram *= 4;
            break;
        case rocfft_precision_double:
            needed_ram *= 8;
            break;
        }
        needed_ram *= nbatch;

        if(verbose > 1)
        {
            std::cout << "required host memory (GB): " << needed_ram * 1e-9 << std::endl;
        }

        if(needed_ram > ramgb * 1e9)
        {
            if(verbose > 2)
            {
                std::cout << "skipped!" << std::endl;
            }
            return;
        }
    }

    // Set up GPU computation:
    if(place == rocfft_placement_inplace)
    {
        const auto stridesize = std::min(istride.size(), ostride.size());
        bool       samestride = true;
        for(int i = 0; i < stridesize; ++i)
        {
            if(istride[i] != ostride[i])
                samestride = false;
        }
        if(!samestride)
        {
            // In-place transforms require identical input and output strides.
            if(verbose)
            {
                std::cout << "istride:";
                for(const auto& i : istride)
                    std::cout << " " << i;
                std::cout << " ostride0:";
                for(const auto& i : ostride)
                    std::cout << " " << i;
                std::cout << " differ; skipped for in-place transforms: skipping test" << std::endl;
            }
            // TODO: mark skipped
            return;
        }
        if((transformType == rocfft_transform_type_real_forward
            || transformType == rocfft_transform_type_real_inverse)
           && (istride[0] != 1 || ostride[0] != 1))
        {
            // In-place real/complex transforms require unit strides.
            if(verbose)
            {
                std::cout << "istride[0]: " << istride[0] << " ostride[0]: " << ostride[0]
                          << " must be unitary for in-place real/complex transforms: skipping test"
                          << std::endl;
            }
            // TODO: mark skipped
            return;
        }

        if((itype == rocfft_array_type_complex_interleaved
            && otype == rocfft_array_type_complex_planar)
           || (itype == rocfft_array_type_complex_planar
               && otype == rocfft_array_type_complex_interleaved))
        {
            if(verbose)
            {
                std::cout << "In-place c2c transforms require identical io types; skipped.\n";
            }
            return;
        }

        if((itype == rocfft_array_type_real && otype == rocfft_array_type_hermitian_planar)
           || (itype == rocfft_array_type_hermitian_planar && otype == rocfft_array_type_real))
        {
            if(verbose)
            {
                std::cout << "In-place real/complex transforms cannot use planar types; skipped.\n";
            }
            return;
        }
    }

    const size_t dim = length.size();

    auto olength = length;
    if(transformType == rocfft_transform_type_real_forward)
        olength[dim - 1] = olength[dim - 1] / 2 + 1;

    auto ilength = length;
    if(transformType == rocfft_transform_type_real_inverse)
        ilength[dim - 1] = ilength[dim - 1] / 2 + 1;

    auto gpu_istride = compute_stride(ilength,
                                      istride,
                                      place == rocfft_placement_inplace
                                          && transformType == rocfft_transform_type_real_forward);

    auto gpu_ostride = compute_stride(olength,
                                      ostride,
                                      place == rocfft_placement_inplace
                                          && transformType == rocfft_transform_type_real_inverse);

    const auto gpu_idist = set_idist(place, transformType, length, gpu_istride);
    const auto gpu_odist = set_odist(place, transformType, length, gpu_ostride);

    rocfft_status fft_status = rocfft_status_success;
    // Transform parameters from row-major to column-major for rocFFT:
    auto gpu_length_cm  = length;
    auto gpu_ilength_cm = ilength;
    auto gpu_olength_cm = olength;
    auto gpu_istride_cm = gpu_istride;
    auto gpu_ostride_cm = gpu_ostride;
    for(int idx = 0; idx < dim / 2; ++idx)
    {
        const auto toidx = dim - idx - 1;
        std::swap(gpu_istride_cm[idx], gpu_istride_cm[toidx]);
        std::swap(gpu_ostride_cm[idx], gpu_ostride_cm[toidx]);
        std::swap(gpu_length_cm[idx], gpu_length_cm[toidx]);
        std::swap(gpu_ilength_cm[idx], gpu_ilength_cm[toidx]);
        std::swap(gpu_olength_cm[idx], gpu_olength_cm[toidx]);
    }

    if(verbose > 1)
    {
        std::cout << gpu_params(gpu_ilength_cm,
                                gpu_istride_cm,
                                gpu_idist,
                                gpu_ostride_cm,
                                gpu_odist,
                                nbatch,
                                precision,
                                place,
                                itype,
                                otype)
                  << std::flush;
    }

    // Create FFT description
    rocfft_plan_description desc = NULL;
    fft_status                   = rocfft_plan_description_create(&desc);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT description creation failure";
    const std::vector<size_t> ioffset = {0, 0};
    const std::vector<size_t> ooffset = {0, 0};
    fft_status                        = rocfft_plan_description_set_data_layout(desc,
                                                         itype,
                                                         otype,
                                                         ioffset.data(),
                                                         ooffset.data(),
                                                         gpu_istride_cm.size(),
                                                         gpu_istride_cm.data(),
                                                         gpu_idist,
                                                         gpu_ostride_cm.size(),
                                                         gpu_ostride_cm.data(),
                                                         gpu_odist);
    EXPECT_TRUE(fft_status == rocfft_status_success)
        << "rocFFT data layout failure: " << fft_status;

    // Create the plan
    rocfft_plan gpu_plan = NULL;
    fft_status           = rocfft_plan_create(&gpu_plan,
                                    place,
                                    transformType,
                                    precision,
                                    gpu_length_cm.size(),
                                    gpu_length_cm.data(),
                                    nbatch,
                                    desc);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT plan creation failure";

    // Create execution info
    rocfft_execution_info info = NULL;
    fft_status                 = rocfft_execution_info_create(&info);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT execution info creation failure";
    size_t workbuffersize = 0;
    fft_status            = rocfft_plan_get_work_buffer_size(gpu_plan, &workbuffersize);
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT get buffer size get failure";

    // Number of value in input and output variables.
    const size_t isize = nbatch * gpu_idist;
    const size_t osize = nbatch * gpu_odist;

    // Sizes of individual input and output variables
    const size_t isize_t = var_size<size_t>(precision, itype);
    const size_t osize_t = var_size<size_t>(precision, otype);

    // Numbers of input and output buffers:
    const int nibuffer
        = (itype == rocfft_array_type_complex_planar || itype == rocfft_array_type_hermitian_planar)
              ? 2
              : 1;
    const int nobuffer
        = (otype == rocfft_array_type_complex_planar || otype == rocfft_array_type_hermitian_planar)
              ? 2
              : 1;

    // Check if the problem fits on the device; if it doesn't skip it.
    if(!vram_fits_problem(nibuffer * isize * isize_t,
                          (place == rocfft_placement_inplace) ? 0 : nobuffer * osize * osize_t,
                          workbuffersize))
    {
        rocfft_plan_destroy(gpu_plan);
        rocfft_plan_description_destroy(desc);
        rocfft_execution_info_destroy(info);

        if(verbose)
        {
            std::cout << "Problem won't fit on device; skipped\n";
        }
        // TODO: mark as skipped via gtest.
        return;
    }

    hipError_t hip_status = hipSuccess;

    // Allocate work memory and associate with the execution info
    gpubuf wbuffer;
    if(workbuffersize > 0)
    {
        hip_status = wbuffer.alloc(workbuffersize);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMalloc failure for work buffer";
        fft_status = rocfft_execution_info_set_work_buffer(info, wbuffer.data(), workbuffersize);
        EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT set work buffer failure";
    }

    // Formatted input data:
    auto gpu_input = allocate_host_buffer<fftwAllocator<char>>(
        precision, itype, length, gpu_istride, gpu_idist, nbatch);

    // Copy from contiguous_input to input.
    copy_buffers(cpu_input.get(),
                 gpu_input,
                 ilength,
                 nbatch,
                 precision,
                 cpu_itype,
                 cpu_istride,
                 cpu_idist,
                 itype,
                 gpu_istride,
                 gpu_idist);

    if(verbose > 4)
    {
        std::cout << "GPU input:\n";
        printbuffer(precision, itype, gpu_input, ilength, gpu_istride, nbatch, gpu_idist);
    }
    if(verbose > 5)
    {
        std::cout << "flat GPU input:\n";
        printbuffer_flat(precision, itype, gpu_input, gpu_idist);
    }

    // GPU input and output buffers:
    auto                ibuffer_sizes = buffer_sizes(precision, itype, gpu_idist, nbatch);
    std::vector<gpubuf> ibuffer(ibuffer_sizes.size());
    std::vector<void*>  pibuffer(ibuffer_sizes.size());
    for(unsigned int i = 0; i < ibuffer.size(); ++i)
    {
        hip_status = ibuffer[i].alloc(ibuffer_sizes[i]);
        ASSERT_TRUE(hip_status == hipSuccess)
            << "hipMalloc failure for input buffer " << i << " size " << ibuffer_sizes[i]
            << gpu_params(gpu_ilength_cm,
                          gpu_istride_cm,
                          gpu_idist,
                          gpu_ostride_cm,
                          gpu_odist,
                          nbatch,
                          precision,
                          place,
                          itype,
                          otype);
        pibuffer[i] = ibuffer[i].data();
    }

    std::vector<gpubuf>  obuffer_data;
    std::vector<gpubuf>* obuffer = &obuffer_data;
    if(place == rocfft_placement_inplace)
    {
        obuffer = &ibuffer;
    }
    else
    {
        auto obuffer_sizes = buffer_sizes(precision, otype, gpu_odist, nbatch);
        obuffer_data.resize(obuffer_sizes.size());
        for(unsigned int i = 0; i < obuffer_data.size(); ++i)
        {
            hip_status = obuffer_data[i].alloc(obuffer_sizes[i]);
            ASSERT_TRUE(hip_status == hipSuccess)
                << "hipMalloc failure for output buffer " << i << " size " << obuffer_sizes[i]
                << gpu_params(gpu_ilength_cm,
                              gpu_istride_cm,
                              gpu_idist,
                              gpu_ostride_cm,
                              gpu_odist,
                              nbatch,
                              precision,
                              place,
                              itype,
                              otype);
        }
    }
    std::vector<void*> pobuffer(obuffer->size());
    for(unsigned int i = 0; i < obuffer->size(); ++i)
    {
        pobuffer[i] = obuffer->at(i).data();
    }

    // Copy the input data to the GPU:
    for(int idx = 0; idx < gpu_input.size(); ++idx)
    {
        hip_status = hipMemcpy(ibuffer[idx].data(),
                               gpu_input[idx].data(),
                               gpu_input[idx].size(),
                               hipMemcpyHostToDevice);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMemcpy failure";
    }

    // Execute the transform:
    fft_status = rocfft_execute(gpu_plan, // plan
                                (void**)pibuffer.data(), // in buffers
                                (void**)pobuffer.data(), // out buffers
                                info); // execution info
    EXPECT_TRUE(fft_status == rocfft_status_success) << "rocFFT plan execution failure";

    // Copy the data back to the host:
    auto gpu_output = allocate_host_buffer<fftwAllocator<char>>(
        precision, otype, olength, gpu_ostride, gpu_odist, nbatch);
    for(int idx = 0; idx < gpu_output.size(); ++idx)
    {
        hip_status = hipMemcpy(gpu_output[idx].data(),
                               obuffer->at(idx).data(),
                               gpu_output[idx].size(),
                               hipMemcpyDeviceToHost);
        EXPECT_TRUE(hip_status == hipSuccess) << "hipMemcpy failure";
    }

    if(verbose > 2)
    {
        std::cout << "GPU output:\n";
        printbuffer(precision, otype, gpu_output, olength, gpu_ostride, nbatch, gpu_odist);
    }
    if(verbose > 5)
    {
        std::cout << "flat GPU output:\n";
        printbuffer_flat(precision, otype, gpu_output, gpu_odist);
    }

    // Compute the Linfinity and L2 norm of the GPU output:
    std::shared_future<VectorNorms> gpu_norm = std::async(std::launch::async, [&]() {
        return norm(gpu_output, olength, nbatch, precision, otype, gpu_ostride, gpu_odist);
    });

    // Compute the l-infinity and l-2 distance between the CPU and GPU output:
    std::vector<std::pair<size_t, size_t>> linf_failures;
    const auto                             total_length
        = std::accumulate(length.begin(), length.end(), 1, std::multiplies<size_t>());
    const double linf_cutoff
        = type_epsilon(precision) * cpu_output_norm.get().l_inf * log(total_length);
    auto diff = distance(cpu_output.get(),
                         gpu_output,
                         olength,
                         nbatch,
                         precision,
                         cpu_otype,
                         cpu_ostride,
                         cpu_odist,
                         otype,
                         gpu_ostride,
                         gpu_odist,
                         linf_failures,
                         linf_cutoff);

    if(verbose > 1)
    {
        std::cout << "GPU output Linf norm: " << gpu_norm.get().l_inf << "\n";
        std::cout << "GPU output L2 norm:   " << gpu_norm.get().l_2 << "\n";
        std::cout << "GPU linf norm failures:";
        std::sort(linf_failures.begin(), linf_failures.end());
        for(const auto& i : linf_failures)
        {
            std::cout << " (" << i.first << "," << i.second << ")";
        }
        std::cout << std::endl;
    }

    EXPECT_TRUE(std::isfinite(gpu_norm.get().l_inf)) << gpu_params(gpu_ilength_cm,
                                                                   gpu_istride_cm,
                                                                   gpu_idist,
                                                                   gpu_ostride_cm,
                                                                   gpu_odist,
                                                                   nbatch,
                                                                   precision,
                                                                   place,
                                                                   itype,
                                                                   otype);
    EXPECT_TRUE(std::isfinite(gpu_norm.get().l_2)) << gpu_params(gpu_ilength_cm,
                                                                 gpu_istride_cm,
                                                                 gpu_idist,
                                                                 gpu_ostride_cm,
                                                                 gpu_odist,
                                                                 nbatch,
                                                                 precision,
                                                                 place,
                                                                 itype,
                                                                 otype);

    if(verbose > 1)
    {
        std::cout << "L2 diff: " << diff.l_2 << "\n";
        std::cout << "Linf diff: " << diff.l_inf << "\n";
    }

    // TODO: handle case where norm is zero?
    EXPECT_TRUE(diff.l_inf < linf_cutoff)
        << "Linf test failed.  Linf:" << diff.l_inf
        << "\tnormalized Linf: " << diff.l_inf / cpu_output_norm.get().l_inf
        << "\tcutoff: " << linf_cutoff
        << gpu_params(gpu_ilength_cm,
                      gpu_istride_cm,
                      gpu_idist,
                      gpu_ostride_cm,
                      gpu_odist,
                      nbatch,
                      precision,
                      place,
                      itype,
                      otype);

    EXPECT_TRUE(diff.l_2 / cpu_output_norm.get().l_2
                < sqrt(log2(total_length)) * type_epsilon(precision))
        << "L2 test failed. L2: " << diff.l_2
        << "\tnormalized L2: " << diff.l_2 / cpu_output_norm.get().l_2
        << "\tepsilon: " << sqrt(log2(total_length)) * type_epsilon(precision)
        << gpu_params(gpu_ilength_cm,
                      gpu_istride_cm,
                      gpu_idist,
                      gpu_ostride_cm,
                      gpu_odist,
                      nbatch,
                      precision,
                      place,
                      itype,
                      otype);

    rocfft_plan_destroy(gpu_plan);
    gpu_plan = NULL;
    rocfft_plan_description_destroy(desc);
    desc = NULL;
    rocfft_execution_info_destroy(info);
    info = NULL;
}

// Test for comparison between FFTW and rocFFT.
TEST_P(accuracy_test, vs_fftw)
{
    const std::vector<size_t>     length        = std::get<0>(GetParam());
    const rocfft_precision        precision     = std::get<1>(GetParam());
    const size_t                  nbatch        = std::get<2>(GetParam());
    const std::vector<size_t>     istride       = std::get<3>(GetParam());
    const std::vector<size_t>     ostride       = std::get<4>(GetParam());
    type_place_io_t               type_place_io = std::get<5>(GetParam());
    const rocfft_transform_type   transformType = std::get<0>(type_place_io);
    const rocfft_result_placement place         = std::get<1>(type_place_io);
    const rocfft_array_type       itype         = std::get<2>(type_place_io);
    const rocfft_array_type       otype         = std::get<3>(type_place_io);

    // NB: Input data is row-major.

    if(ramgb > 0)
    {
        // Estimate the amount of memory needed, and skip if it's more than we allow.

        // Host input, output, and input copy, gpu input and output: 5 buffers.
        // This test assumes that all buffers are contiguous; other cases are dealt with when they
        // are called.
        // FFTW may require work memory; this is not accounted for.
        size_t needed_ram
            = 5 * std::accumulate(length.begin(), length.end(), 1, std::multiplies<size_t>());

        // Account for precision and data type:
        if(transformType != rocfft_transform_type_real_forward
           || transformType != rocfft_transform_type_real_inverse)
        {
            needed_ram *= 2;
        }
        switch(precision)
        {
        case rocfft_precision_single:
            needed_ram *= 4;
            break;
        case rocfft_precision_double:
            needed_ram *= 8;
            break;
        }

        if(needed_ram > ramgb * 1e9)
        {
            GTEST_SKIP();
            return;
        }
    }
    auto cpu = accuracy_test::compute_cpu_fft(length, nbatch, precision, transformType);

    // Set up GPU computations:
    if(verbose)
    {
        print_params(
            length, istride, ostride, nbatch, place, precision, transformType, itype, otype);
    }

    rocfft_transform(length,
                     istride,
                     ostride,
                     nbatch,
                     precision,
                     transformType,
                     itype,
                     otype,
                     place,
                     cpu.istride,
                     cpu.ostride,
                     cpu.idist,
                     cpu.odist,
                     cpu.itype,
                     cpu.otype,
                     cpu.input,
                     cpu.output,
                     ramgb,
                     cpu.output_norm);

    auto cpu_input_norm = cpu.input_norm.get();
    ASSERT_TRUE(std::isfinite(cpu_input_norm.l_2));
    ASSERT_TRUE(std::isfinite(cpu_input_norm.l_inf));

    auto cpu_output_norm = cpu.output_norm.get();
    ASSERT_TRUE(std::isfinite(cpu_output_norm.l_2));
    ASSERT_TRUE(std::isfinite(cpu_output_norm.l_inf));

    SUCCEED();
}
