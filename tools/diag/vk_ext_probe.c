// Which Vulkan device extensions does this driver advertise?
//
// Enough of vulkaninfo to answer one question without building Vulkan-Tools:
// the driver's identity and its device extension list, with the handful that
// decide capability in this codebase called out. Instance-only work, so it
// needs no surface, no display and no DRM master.
//
//     cc -O2 -Wall -Wextra -o vk_ext_probe vk_ext_probe.c -lvulkan
//     ./vk_ext_probe [--all]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

// The ones this shell's behavior turns on.
static const char* kOfInterest[] = {
    "VK_KHR_external_semaphore_fd",     "VK_KHR_external_fence_fd",
    "VK_KHR_external_memory_fd",        "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier", "VK_KHR_synchronization2",
    "VK_EXT_queue_family_foreign",
};

int main(int argc, char** argv) {
  const int show_all = argc > 1 && strcmp(argv[1], "--all") == 0;

  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "vk_ext_probe",
                           .apiVersion = VK_API_VERSION_1_0};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app};
  VkInstance instance = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, NULL, &instance);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "vkCreateInstance failed (%d)\n", r);
    return 1;
  }

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, NULL);
  if (n == 0) {
    fprintf(stderr, "no Vulkan physical device\n");
    return 1;
  }
  VkPhysicalDevice* pds = calloc(n, sizeof(*pds));
  vkEnumeratePhysicalDevices(instance, &n, pds);
  printf("physical devices: %u\n", n);

  int missing_any = 0;
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pds[i], &p);
    printf("\n[%u] %s\n", i, p.deviceName);
    printf("    apiVersion    %u.%u.%u\n", VK_VERSION_MAJOR(p.apiVersion),
           VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    printf("    driverVersion %u  vendorID 0x%x  deviceID 0x%x\n",
           p.driverVersion, p.vendorID, p.deviceID);

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pds[i], NULL, &en, NULL);
    VkExtensionProperties* eps = calloc(en ? en : 1, sizeof(*eps));
    vkEnumerateDeviceExtensionProperties(pds[i], NULL, &en, eps);
    printf("    device extensions: %u\n", en);

    for (size_t k = 0; k < sizeof(kOfInterest) / sizeof(*kOfInterest); k++) {
      int found = 0;
      for (uint32_t e = 0; e < en; e++) {
        if (strcmp(eps[e].extensionName, kOfInterest[k]) == 0) {
          found = 1;
          break;
        }
      }
      printf("      %-36s %s\n", kOfInterest[k], found ? "yes" : "NO");
      if (!found) {
        missing_any = 1;
      }
    }
    if (show_all) {
      for (uint32_t e = 0; e < en; e++) {
        printf("      . %s (rev %u)\n", eps[e].extensionName,
               eps[e].specVersion);
      }
    }
    free(eps);
  }
  free(pds);
  vkDestroyInstance(instance, NULL);
  return missing_any ? 2 : 0;
}
