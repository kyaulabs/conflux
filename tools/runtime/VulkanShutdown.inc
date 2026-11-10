// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdio>

extern "C" bool ggml_vk_conflux_shutdown() {
    if (!vk_instance.instance) {
        return true;
    }

    bool all_devices_idle = true;
    for (const auto & device : vk_instance.devices) {
        if (!device || !device->device) {
            continue;
        }
        const VkResult idle = vkDeviceWaitIdle(static_cast<VkDevice>(device->device));
        if (idle != VK_SUCCESS && idle != VK_ERROR_DEVICE_LOST) {
            std::fprintf(stderr, "conflux_vulkan_shutdown: wait-idle result=%d\n", static_cast<int>(idle));
            return false;
        }
        all_devices_idle = all_devices_idle && idle == VK_SUCCESS;
        // The shared staging buffer itself owns a reference to this device.
        ggml_vk_destroy_buffer(device->sync_staging);
    }

    for (const auto & device : vk_instance.devices) {
        if (!device) {
            continue;
        }
        const auto * buffer_type = static_cast<const ggml_backend_vk_buffer_type_context *>(device->buffer_type.context);
        // The registry and its buffer-type context are the only expected owners.
        if ((buffer_type && buffer_type->device.get() != device.get()) ||
            device.use_count() != (buffer_type ? 2 : 1)) {
            std::fprintf(stderr, "conflux_vulkan_shutdown: unexpected device owners=%ld buffer_type=%d\n",
                         device.use_count(), buffer_type != nullptr);
            return false;
        }
    }
    for (auto & device : vk_instance.devices) {
        if (device) {
            delete static_cast<ggml_backend_vk_buffer_type_context *>(device->buffer_type.context);
            device->buffer_type.context = nullptr;
            device.reset();
        }
    }
    vkDestroyInstance(static_cast<VkInstance>(vk_instance.instance), nullptr);
    vk_instance.instance = nullptr;
    vk_instance_initialized = false;
    std::fprintf(stderr, "conflux_vulkan_shutdown: device and instance released, idle=%d\n", all_devices_idle);
    return all_devices_idle;
}
