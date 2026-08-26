#ifndef AutoGrowBufferStream_h
#define AutoGrowBufferStream_h

// ---------------------------------------------------------------------------
// AutoGrowBufferStream -- the Stream PubSubClient writes each MQTT payload into.
// (originally from the XTouch project)
//
// Fixes over upstream (docs/REVIEW.md #14):
//   * _len / buffer_size are size_t, so the 64 kB guard actually fires and the
//     length cannot wrap at 65535 (an X1 pushes ~16 kB per second).
//   * the buffer always has room for a terminating NUL, so get_string() can no
//     longer write one byte past the allocation.
//   * flush() only resets the length; the capacity is kept, which removes a
//     realloc-down/realloc-up pair per message (heap churn at 1 msg/s).
//
// Threading: written and read exclusively by mqttTask.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Stream.h>

#define BUFFER_INCREMENTS 512
#define MAX_BUFFER_SIZE 65536

class AutoGrowBufferStream : public Stream
{
private:
    size_t _len;
    size_t buffer_size;
    char *_buffer;
    bool _overflow;

    // Grow to at least `needed` bytes (payload + terminating NUL).
    bool ensure(size_t needed)
    {
        if (needed <= buffer_size)
            return true;
        size_t newSize = buffer_size;
        while (newSize < needed)
            newSize += BUFFER_INCREMENTS;
        if (newSize > MAX_BUFFER_SIZE + 1)
            newSize = MAX_BUFFER_SIZE + 1;
        if (newSize < needed)
            return false;
        char *tmp = (char *)realloc(_buffer, newSize);
        if (tmp == NULL)
            return false;
        _buffer = tmp;
        buffer_size = newSize;
        return true;
    }

public:
    AutoGrowBufferStream()
    {
        _len = 0;
        _overflow = false;
        buffer_size = BUFFER_INCREMENTS;
        _buffer = (char *)malloc(buffer_size);
        if (_buffer == NULL)
            buffer_size = 0;
    }

    ~AutoGrowBufferStream()
    {
        free(_buffer);
    }

    virtual size_t write(uint8_t byte)
    {
        if (_len + 1 >= MAX_BUFFER_SIZE)
        {
            // Payload larger than we will ever parse: drop the rest, flag it.
            _overflow = true;
            return 0;
        }
        if (!ensure(_len + 2)) // payload byte + room for the NUL
        {
            _overflow = true;
            return 0;
        }
        _buffer[_len++] = (char)byte;
        return 1;
    }

    virtual int read() { return -1; }
    virtual int available() { return 0; }
    int peek() { return -1; }

    // Reset for the next message; the capacity is intentionally kept.
    virtual void flush()
    {
        _len = 0;
        _overflow = false;
    }

    size_t current_length() const { return _len; }
    size_t capacity() const { return buffer_size; }
    bool overflowed() const { return _overflow; }
    const char *get_buffer() const { return _buffer; }

    // NUL-terminated view of the payload (always inside the allocation).
    const char *get_string()
    {
        if (_buffer == NULL)
            return "";
        if (_len < buffer_size)
            _buffer[_len] = '\0';
        else
            _buffer[buffer_size - 1] = '\0';
        return _buffer;
    }

    using Print::write;
};

#endif
