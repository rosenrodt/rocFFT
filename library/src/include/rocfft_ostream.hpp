/******************************************************************************
* Copyright (c) 2016 - present Advanced Micro Devices, Inc. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*******************************************************************************/

#ifndef _ROCFFT_OSTREAM_HPP_
#define _ROCFFT_OSTREAM_HPP_

#include "rocfft.h"
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

/*****************************************************************************
 * rocFFT output streams                                                    *
 *****************************************************************************/

#define rocfft_cout (rocfft_ostream::cout())
#define rocfft_cerr (rocfft_ostream::cerr())

/***************************************************************************
 * The rocfft_ostream class performs atomic IO on log files, and provides *
 * consistent formatting                                                   *
 ***************************************************************************/
class rocfft_ostream
{
    /**************************************************************************
     * The worker class sets up a worker thread for writing to log files. Two *
     * files are considered the same if they have the same device ID / inode. *
     **************************************************************************/
    class worker
    {
        // task_t represents a payload of data and a promise to finish
        class task_t
        {
            std::string        str;
            std::promise<void> promise;

        public:
            // The task takes ownership of the string payload and promise
            task_t(std::string&& str, std::promise<void>&& promise)
                : str(std::move(str))
                , promise(std::move(promise))
            {
            }

            // Notify the future to wake up
            void set_value()
            {
                promise.set_value();
            }

            // Size of the string payload
            size_t size() const
            {
                return str.size();
            }

            // Data of the string payload
            const char* data() const
            {
                return str.data();
            }
        };

        // FILE is used for safety in the presence of signals
        FILE* file = nullptr;

        // This worker's thread
        std::thread thread;

        // Condition variable for worker notification
        std::condition_variable cond;

        // Mutex for this thread's queue
        std::mutex mutex;

        // Queue of tasks
        std::queue<task_t> queue;

        // Worker thread which waits for and handles tasks sequentially
        void thread_function();

    public:
        // Worker constructor creates a worker thread for a raw filehandle
        explicit worker(int fd);

        // Send a string to be written
        void send(std::string);

        // Destroy a worker when all std::shared_ptr references to it are gone
        ~worker()
        {
            // Tell worker thread to exit, by sending it an empty string
            send({});

            // Close the FILE
            if(file)
                fclose(file);
        }
    };

    // Two filehandles point to the same file if they share the same (std_dev, std_ino).

    // Initial slice of struct stat which contains device ID and inode
    struct file_id_t
    {
        dev_t st_dev; // ID of device containing file
        ino_t st_ino; // Inode number
    };

    // Compares device IDs and inodes for map containers
    struct file_id_less
    {
        bool operator()(const file_id_t& lhs, const file_id_t& rhs) const
        {
            return lhs.st_ino < rhs.st_ino || (lhs.st_ino == rhs.st_ino && lhs.st_dev < rhs.st_dev);
        }
    };

    // Map from file_id to a worker shared_ptr
    // Implemented as singleton to avoid the static initialization order fiasco
    static auto& map()
    {
        static std::map<file_id_t, std::shared_ptr<worker>, file_id_less> map;
        return map;
    }

    // Mutex for accessing the map
    // Implemented as singleton to avoid the static initialization order fiasco
    static auto& map_mutex()
    {
        static std::recursive_mutex map_mutex;
        return map_mutex;
    }

    // Output buffer for formatted IO
    std::ostringstream os;

    // Worker thread for accepting tasks
    std::shared_ptr<worker> worker_ptr;

    // Get worker for file descriptor
    static std::shared_ptr<worker> get_worker(int fd);

public:
    // Default constructor is a std::ostringstream with no worker
    rocfft_ostream() = default;

    // Move constructor
    rocfft_ostream(rocfft_ostream&&) = default;

    // Copy constructor is deleted
    rocfft_ostream(const rocfft_ostream&) = delete;

    // Move assignment
    rocfft_ostream& operator=(rocfft_ostream&&) = default;

    // Copy assignment is deleted
    rocfft_ostream& operator=(const rocfft_ostream&) = delete;

    // Construct from a file descriptor, which is duped
    explicit rocfft_ostream(int fd);

    // Construct from a C filename
    explicit rocfft_ostream(const char* filename);

    // Construct from a std::string filename
    explicit rocfft_ostream(const std::string& filename)
        : rocfft_ostream(filename.c_str())
    {
    }

    // Convert stream output to string
    std::string str() const
    {
        return os.str();
    }

    // Clear the buffer
    void clear()
    {
        os.clear();
        os.str({});
    }

    // Flush the output
    void flush();

    // Destroy the rocfft_ostream
    virtual ~rocfft_ostream()
    {
        flush(); // Flush any pending IO
    }

    // Implemented as singleton to avoid the static initialization order fiasco
    static rocfft_ostream& cout()
    {
        thread_local rocfft_ostream cout{STDOUT_FILENO};
        return cout;
    }

    // Implemented as singleton to avoid the static initialization order fiasco
    static rocfft_ostream& cerr()
    {
        thread_local rocfft_ostream cerr{STDERR_FILENO};
        return cerr;
    }

    // Abort function which safely flushes all IO
    friend void rocfft_abort_once();

    /*************************************************************************
     * Non-member friend functions for formatted output                      *
     *************************************************************************/

    // Default output
    template <typename T>
    friend rocfft_ostream& operator<<(rocfft_ostream& os, T&& x)
    {
        os.os << std::forward<T>(x);
        return os;
    }

    // Floating-point output
    friend rocfft_ostream& operator<<(rocfft_ostream& os, double x);
    friend rocfft_ostream& operator<<(rocfft_ostream& os, float f);

    // Integer output
    friend rocfft_ostream& operator<<(rocfft_ostream& os, int32_t x)
    {
        os.os << x;
        return os;
    }
    friend rocfft_ostream& operator<<(rocfft_ostream& os, uint32_t x)
    {
        os.os << x;
        return os;
    }
    friend rocfft_ostream& operator<<(rocfft_ostream& os, int64_t x)
    {
        os.os << x;
        return os;
    }
    friend rocfft_ostream& operator<<(rocfft_ostream& os, uint64_t x)
    {
        os.os << x;
        return os;
    }

    // bool output
    friend rocfft_ostream& operator<<(rocfft_ostream& os, bool b);

    // Character output
    friend rocfft_ostream& operator<<(rocfft_ostream& os, char c);

    // String output
    friend rocfft_ostream& operator<<(rocfft_ostream& os, const char* s);
    friend rocfft_ostream& operator<<(rocfft_ostream& os, const std::string& s);

    // Transfer rocfft_ostream to rocfft_ostream
    friend rocfft_ostream& operator<<(rocfft_ostream& os, const rocfft_ostream& str)
    {
        return os << str.str();
    }

    friend std::ostream& operator<<(std::ostream& os, const rocfft_ostream& str);

    // IO Manipulators
    friend rocfft_ostream& operator<<(rocfft_ostream& os, std::ostream& (*pf)(std::ostream&));

    // rocfft-specific enums and types
    friend rocfft_ostream& operator<<(rocfft_ostream& os, rocfft_transform_type type);
    friend rocfft_ostream& operator<<(rocfft_ostream& os, rocfft_precision precision);
    friend rocfft_ostream& operator<<(rocfft_ostream& os, rocfft_result_placement placement);
    friend rocfft_ostream& operator<<(rocfft_ostream& os, rocfft_array_type type);
    // array of size_t's with length of that array, for strides,
    // offsets, etc
    friend rocfft_ostream& operator<<(rocfft_ostream& os, std::pair<const size_t*, size_t> array);
};

#endif
