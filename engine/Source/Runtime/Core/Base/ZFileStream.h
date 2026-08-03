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
