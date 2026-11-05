#pragma once
// ZFileStream — C FILE*-based replacement for std::ifstream/std::ofstream
//
// On iOS, when ZRuntimeShared.framework is loaded by UnityFramework via
// dlopen(), the C++ ABI symbols (VTT/vtable) for std::basic_ifstream,
// std::basic_ofstream, std::basic_stringstream, std::length_error, and
// std::out_of_range are NOT resolvable by dyld — even with -flat_namespace.
// Unity's embedded libc++ does not export them, and the system libc++.1.dylib
// is apparently not visible in the process's flat namespace.
//
// The workaround: replace all std::ifstream / std::ofstream usage in Runtime
// code with plain C FILE* I/O. This removes the dependency on these
// problematic symbol categories entirely.
//

#include <cstdio>
#include <string>

namespace zcore {

/**
 * Read at most `size` bytes from the head of `path`. Returns the number of
 * bytes actually read (0 when the file cannot be opened). Binary mode.
 */
inline size_t ReadFilePrefix(const char* path, void* buf, size_t size) {
    if (path == nullptr || buf == nullptr || size == 0) return 0;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return 0;
    const size_t read_bytes = std::fread(buf, 1, size, f);
    std::fclose(f);
    return read_bytes;
}

inline size_t ReadFilePrefix(const std::string& path, void* buf, size_t size) {
    return ReadFilePrefix(path.c_str(), buf, size);
}

/**
 * Read the entire file into `out` (binary, no NUL terminator appended).
 * Returns false when the file cannot be opened / is empty.
 */
inline bool ReadWholeFile(const char* path, std::string& out) {
    out.clear();
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long size = std::ftell(f);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    std::rewind(f);
    out.resize(static_cast<size_t>(size));
    const size_t read_bytes = std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    if (read_bytes != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

inline bool ReadWholeFile(const std::string& path, std::string& out) {
    return ReadWholeFile(path.c_str(), out);
}

/**
 * Write `size` bytes at the head of `path`, truncating it first.
 * Returns false when the file cannot be opened or the write is short.
 */
inline bool WriteWholeFile(const char* path, const void* data, size_t size) {
    if (path == nullptr) return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const size_t written = (size == 0) ? 0 : std::fwrite(data, 1, size, f);
    std::fclose(f);
    return written == size;
}

/**
 * Minimal RAII file reader (binary mode).
 * Drop-in for the common std::ifstream read pattern.
 */
class ZFileReader {
public:
    explicit ZFileReader(const std::string& path) {
        m_file = fopen(path.c_str(), "rb");
    }

    explicit ZFileReader(const char* path) {
        m_file = fopen(path, "rb");
    }

    ~ZFileReader() { close(); }

    ZFileReader(const ZFileReader&) = delete;
    ZFileReader& operator=(const ZFileReader&) = delete;

    bool isOpen() const { return m_file != nullptr; }

    size_t read(void* buf, size_t size) {
        if (!m_file) return 0;
        return fread(buf, 1, size, m_file);
    }

    int getc() {
        if (!m_file) return EOF;
        return fgetc(m_file);
    }

    bool getline(char* buf, size_t bufsz) {
        if (!m_file) return false;
        return fgets(buf, static_cast<int>(bufsz), m_file) != nullptr;
    }

    long tell() {
        if (!m_file) return -1;
        return ftell(m_file);
    }

    int seek(long offset, int origin) {
        if (!m_file) return -1;
        return fseek(m_file, offset, origin);
    }

    bool eof() const {
        if (!m_file) return true;
        return feof(m_file) != 0;
    }

    void close() {
        if (m_file) {
            fclose(m_file);
            m_file = nullptr;
        }
    }

    FILE* handle() { return m_file; }

private:
    FILE* m_file = nullptr;
};

/**
 * Minimal RAII file writer (binary mode).
 * Drop-in for the common std::ofstream write pattern.
 */
class ZFileWriter {
public:
    explicit ZFileWriter(const std::string& path) {
        m_file = fopen(path.c_str(), "wb");
    }

    explicit ZFileWriter(const char* path) {
        m_file = fopen(path, "wb");
    }

    ~ZFileWriter() { close(); }

    ZFileWriter(const ZFileWriter&) = delete;
    ZFileWriter& operator=(const ZFileWriter&) = delete;

    bool isOpen() const { return m_file != nullptr; }

    size_t write(const void* buf, size_t size) {
        if (!m_file) return 0;
        return fwrite(buf, 1, size, m_file);
    }

    void putc(int c) {
        if (m_file) fputc(c, m_file);
    }

    void flush() {
        if (m_file) fflush(m_file);
    }

    void close() {
        if (m_file) {
            fclose(m_file);
            m_file = nullptr;
        }
    }

    FILE* handle() { return m_file; }

private:
    FILE* m_file = nullptr;
};

} // namespace zcore
