#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A memory card backed by a raw image file. A game saves 128 bytes at a time, hundreds of
// times for one save, and writing the whole image after each of those held the guest up for
// a frame or more. So the card's own thread writes the file once the guest has left the card
// alone for `quiet`, which is once a save, and shutdown() writes what's left on the way out.
// Each write goes to a temporary file that's renamed over the card's, so the file is always
// a whole card.
class MemCard {
public:
    static constexpr std::chrono::milliseconds kQuiet{1000};

    MemCard(const std::string& path, uint32_t size_mbit, std::chrono::milliseconds quiet = kQuiet);
    ~MemCard();
    MemCard(const MemCard&) = delete;
    MemCard& operator=(const MemCard&) = delete;

    void select();
    // Returns true if the card raises its (EXI) interrupt, e.g. after erase/program.
    bool deselect();
    uint8_t transfer(uint8_t in, bool reading);
    // Stops the card's thread and writes the file on the calling thread if it lacks a write.
    // A write after this is saved as it comes.
    void shutdown();
    // How many times the card's file has been written.
    uint64_t saves() const { return saves_.load(); }
    void format();
    void write_serial(uint8_t* header);

private:
    using Clock = std::chrono::steady_clock;
    bool write_file(const std::vector<uint8_t>& image);
    void saver_main();

    std::string path_;
    uint32_t size_mbit_;
    std::chrono::milliseconds quiet_;
    std::vector<uint8_t> data_;
    uint8_t status_;
    uint32_t pos_ = 0;
    uint8_t cmd_ = 0;
    uint32_t addr_ = 0;
    std::vector<uint8_t> prog_;
    bool int_enabled_ = true;

    // mutex_ guards the guest's writes to data_ against the saver's copy of it, and the
    // fields from here on.
    std::mutex mutex_;
    std::condition_variable cv_;
    bool dirty_ = false;     // data_ has writes the file lacks
    bool stop_ = false;      // shutdown() has begun
    bool stopped_ = false;   // and the saver is gone: write the file at each write
    Clock::time_point last_write_;
    std::vector<uint8_t> snapshot_;
    std::atomic<uint64_t> saves_{0};
    std::thread saver_;
};
