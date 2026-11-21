// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <vulkan/vulkan_core.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <new>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace conflux {

class VulkanPipelineCache {
public:
    VkPipelineCache get(VkDevice device, const VkPhysicalDeviceProperties& properties)
    {
        std::call_once(initialize_, [&] {
            directory_ = openDirectory();
            if (directory_ < 0)
                return;

            char uuid[VK_UUID_SIZE * 2 + 1]{};
            for (std::size_t index = 0; index < VK_UUID_SIZE; ++index)
                std::snprintf(uuid + index * 2, 3, "%02x", unsigned(properties.pipelineCacheUUID[index]));
            std::snprintf(name_, sizeof(name_), "%08x-%08x-%s.bin", properties.vendorID, properties.deviceID, uuid);

            const auto data = load(properties);
            VkPipelineCacheCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
            info.initialDataSize = data.size();
            info.pInitialData = data.empty() ? nullptr : data.data();
            VkPipelineCache created = VK_NULL_HANDLE;
            auto status = vkCreatePipelineCache(device, &info, nullptr, &created);
            if (status != VK_SUCCESS && !data.empty()) {
                info.initialDataSize = 0;
                info.pInitialData = nullptr;
                status = vkCreatePipelineCache(device, &info, nullptr, &created);
            }
            if (status == VK_SUCCESS)
                cache_ = created;
        });
        return cache_;
    }

    // Called after inference and pipeline creation are finished, while the device is still live.
    void save(VkDevice device, const VkPhysicalDeviceProperties& properties) const noexcept
    {
        if (!cache_)
            return;
        try {
            std::size_t size = 0;
            if (vkGetPipelineCacheData(device, cache_, &size, nullptr) != VK_SUCCESS ||
                size < vulkanHeaderSize || size > payloadLimit)
                return;
            std::vector<unsigned char> data(size);
            if (vkGetPipelineCacheData(device, cache_, &size, data.data()) != VK_SUCCESS || size > data.size())
                return;
            data.resize(size);
            if (!matchesDevice(data, properties))
                return;
            const Record record{size, fingerprint(data)};
            if (flock(directory_, LOCK_EX | LOCK_NB) == 0) {
                publish(record, data);
                (void)flock(directory_, LOCK_UN);
            }
        } catch (const std::bad_alloc&) {
            // A cache write is optional.
        }
    }

    void destroy(VkDevice device) noexcept
    {
        if (cache_)
            vkDestroyPipelineCache(device, cache_, nullptr);
        if (directory_ >= 0)
            close(directory_);
        cache_ = VK_NULL_HANDLE;
        directory_ = -1;
    }

private:
    static constexpr std::size_t vulkanHeaderSize = 32;
    static constexpr std::size_t payloadLimit = 32 * 1024 * 1024;
    struct Record {
        std::uint64_t size;
        std::uint64_t fingerprint;
    };
    static_assert(sizeof(Record) == 16);

    static int openDirectory() noexcept
    {
        try {
            std::filesystem::path root;
            if (const char* path = std::getenv("XDG_CACHE_HOME"); path && path[0] == '/') {
                root = path;
            } else if (const char* home = std::getenv("HOME"); home && home[0] == '/') {
                root = std::filesystem::path(home) / ".cache";
            } else {
                return -1;
            }
            root /= "conflux";
            std::error_code error;
            std::filesystem::create_directories(root, error);
            if (error)
                return -1;
            root /= "vulkan-pipelines-v1";
            if (mkdir(root.c_str(), 0700) < 0 && errno != EEXIST)
                return -1;
            const int directory = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (directory < 0)
                return -1;
            struct stat info{};
            if (fstat(directory, &info) == 0 && info.st_uid == geteuid() && (info.st_mode & 0777) == 0700)
                return directory;
            close(directory);
        } catch (const std::exception&) {
            // Cache paths may be unavailable in restricted runtime environments.
        }
        return -1;
    }

    static std::uint64_t fingerprint(const std::vector<unsigned char>& data) noexcept
    {
        // FNV-1a detects incomplete or corrupted writes within the private cache.
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto byte : data)
            hash = (hash ^ byte) * 1099511628211ULL;
        return hash;
    }

    static bool matchesDevice(const std::vector<unsigned char>& data,
                              const VkPhysicalDeviceProperties& properties) noexcept
    {
        if (data.size() < vulkanHeaderSize)
            return false;
        const auto field = [&](std::size_t index) {
            return std::uint32_t(data[index]) | (std::uint32_t(data[index + 1]) << 8) |
                (std::uint32_t(data[index + 2]) << 16) | (std::uint32_t(data[index + 3]) << 24);
        };
        return field(0) == vulkanHeaderSize && field(4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
            field(8) == properties.vendorID && field(12) == properties.deviceID &&
            std::memcmp(data.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }

    std::vector<unsigned char> load(const VkPhysicalDeviceProperties& properties) const
    {
        const int fd = openat(directory_, name_, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return {};
        auto* file = fdopen(fd, "rb");
        if (!file) {
            close(fd);
            return {};
        }
        std::vector<unsigned char> data;
        try {
            struct stat info{};
            Record record{};
            if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
                (info.st_mode & 0777) == 0600 && info.st_nlink == 1 &&
                info.st_size >= static_cast<off_t>(sizeof(Record) + vulkanHeaderSize) &&
                info.st_size <= static_cast<off_t>(sizeof(Record) + payloadLimit) &&
                std::fread(&record, sizeof(record), 1, file) == 1 &&
                record.size == static_cast<std::uint64_t>(info.st_size) - sizeof(Record)) {
                data.resize(record.size);
                if (std::fread(data.data(), 1, data.size(), file) != data.size() ||
                    !matchesDevice(data, properties) || fingerprint(data) != record.fingerprint)
                    data.clear();
            }
        } catch (const std::bad_alloc&) {
            data.clear();
        }
        std::fclose(file);
        return data;
    }

    void publish(const Record& record, const std::vector<unsigned char>& data) const noexcept
    {
        char temporary[sizeof(name_) + 4]{};
        std::snprintf(temporary, sizeof(temporary), "%s.tmp", name_);
        // A writer may have been killed before rename. The directory lock serializes writers.
        (void)unlinkat(directory_, temporary, 0);
        const int fd = openat(directory_, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return;
        auto* file = fdopen(fd, "wb");
        if (!file) {
            close(fd);
            unlinkat(directory_, temporary, 0);
            return;
        }
        const bool wrote = std::fwrite(&record, sizeof(record), 1, file) == 1 &&
            std::fwrite(data.data(), 1, data.size(), file) == data.size();
        const bool closed = std::fclose(file) == 0;
        if (!wrote || !closed || renameat(directory_, temporary, directory_, name_) < 0)
            unlinkat(directory_, temporary, 0);
    }

    std::once_flag initialize_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    int directory_ = -1;
    char name_[64]{};
};

} // namespace conflux
