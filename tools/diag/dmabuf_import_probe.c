// Can this driver's Vulkan import a dma-buf its own GBM allocated for the
// same image?
//
// Allocates through GBM with an explicit modifier, exports the dma-buf, builds
// a VkImage over that exact layout, and imports it at the only allocationSize a
// dedicated allocation may legally use -- VkMemoryRequirements::size. Then, if
// that is refused, retries a few bytes short to show what the refusal is about.
//
// Nothing here is specific to one driver. On a driver that charges an import
// for internal scratch padding the dma-buf cannot carry, the first import fails
// and the short one succeeds, and the "spare" column shows the rule: it is the
// bytes left over in the image's last page that decide it, not the extent, the
// format or the modifier.
//
// Headless. No surface, no display, no DRM master -- it runs over plain SSH on
// a render node.
//
//     cc -O2 -Wall -Wextra -o dmabuf_import_probe dmabuf_import_probe.c
//         $(pkg-config --cflags --libs gbm libdrm) -lvulkan
//     ./dmabuf_import_probe

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <gbm.h>
#include <vulkan/vulkan.h>

#define MAX_PLANES 4

static const char* kDeviceExts[] = {
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
};

struct vk {
  VkInstance instance;
  VkPhysicalDevice pd;
  VkDevice dev;
  char driver[VK_MAX_DRIVER_NAME_SIZE];
  char driver_info[VK_MAX_DRIVER_INFO_SIZE];
  char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

static const char* vkstr(VkResult r) {
  switch (r) {
    case VK_SUCCESS:
      return "VK_SUCCESS";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
      return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
      return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:
      return "VkResult";
  }
}

static bool vk_init(struct vk* v) {
  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "dmabuf_import_probe",
                           .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                              .pApplicationInfo = &app};
  VkResult r = vkCreateInstance(&ici, NULL, &v->instance);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "vkCreateInstance: %s (%d)\n", vkstr(r), r);
    return false;
  }

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(v->instance, &n, NULL);
  if (n == 0) {
    fprintf(stderr, "no Vulkan physical device\n");
    return false;
  }
  VkPhysicalDevice* pds = calloc(n, sizeof(*pds));
  vkEnumeratePhysicalDevices(v->instance, &n, pds);

  // First device that carries all three extensions. A software device would do
  // the arithmetic but not the driver's, so skip CPU implementations.
  for (uint32_t i = 0; i < n; i++) {
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pds[i], NULL, &en, NULL);
    VkExtensionProperties* eps = calloc(en, sizeof(*eps));
    vkEnumerateDeviceExtensionProperties(pds[i], NULL, &en, eps);
    size_t have = 0;
    for (uint32_t e = 0; e < en; e++) {
      for (size_t w = 0; w < sizeof(kDeviceExts) / sizeof(*kDeviceExts); w++) {
        if (strcmp(eps[e].extensionName, kDeviceExts[w]) == 0) {
          have++;
        }
      }
    }
    free(eps);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pds[i], &props);
    if (have == sizeof(kDeviceExts) / sizeof(*kDeviceExts) &&
        props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
      v->pd = pds[i];
      break;
    }
  }
  free(pds);
  if (v->pd == VK_NULL_HANDLE) {
    fprintf(stderr, "no device with %s + %s + %s\n", kDeviceExts[0],
            kDeviceExts[1], kDeviceExts[2]);
    return false;
  }

  VkPhysicalDeviceDriverProperties drv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceProperties2 p2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
  vkGetPhysicalDeviceProperties2(v->pd, &p2);
  snprintf(v->driver, sizeof(v->driver), "%s", drv.driverName);
  snprintf(v->driver_info, sizeof(v->driver_info), "%s", drv.driverInfo);
  snprintf(v->device_name, sizeof(v->device_name), "%s",
           p2.properties.deviceName);

  float prio = 1.0f;
  VkDeviceQueueCreateInfo q = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &prio};
  VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &q,
      .enabledExtensionCount = sizeof(kDeviceExts) / sizeof(*kDeviceExts),
      .ppEnabledExtensionNames = kDeviceExts};
  r = vkCreateDevice(v->pd, &dci, NULL, &v->dev);
  if (r != VK_SUCCESS) {
    fprintf(stderr, "vkCreateDevice: %s (%d)\n", vkstr(r), r);
    return false;
  }
  return true;
}

// Modifiers the driver will import for @usage, in the order it lists them.
static uint32_t importable_modifiers(struct vk* v,
                                     VkFormat format,
                                     VkImageUsageFlags usage,
                                     uint64_t* out,
                                     uint32_t* out_planes,
                                     uint32_t max) {
  VkDrmFormatModifierPropertiesListEXT list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
  VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
                            .pNext = &list};
  vkGetPhysicalDeviceFormatProperties2(v->pd, format, &fp);
  if (list.drmFormatModifierCount == 0) {
    return 0;
  }
  VkDrmFormatModifierPropertiesEXT* props =
      calloc(list.drmFormatModifierCount, sizeof(*props));
  list.pDrmFormatModifierProperties = props;
  vkGetPhysicalDeviceFormatProperties2(v->pd, format, &fp);

  uint32_t found = 0;
  for (uint32_t i = 0; i < list.drmFormatModifierCount && found < max; i++) {
    // The authoritative question is not "does this tiling support the usage"
    // but "will an external dma-buf of this modifier import with it".
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &ext,
        .drmFormatModifier = props[i].drmFormatModifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &mod,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage};
    VkExternalImageFormatProperties efp = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &efp};
    if (vkGetPhysicalDeviceImageFormatProperties2(v->pd, &info, &ifp) !=
        VK_SUCCESS) {
      continue;
    }
    if (!(efp.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
      continue;
    }
    out[found] = props[i].drmFormatModifier;
    out_planes[found] = props[i].drmFormatModifierPlaneCount;
    found++;
  }
  free(props);
  return found;
}

// Mesa 26.1 narrowed the read-ahead to images that declare TRANSFER_SRC
// (get_image_memory_requirements), so which usage bits an imported image asks
// for decides whether it is charged. Keep it selectable rather than baked in:
// the same extent can pass or fail on that bit alone.
struct usage_name {
  const char* name;
  VkImageUsageFlags bit;
};

static const struct usage_name kUsageNames[] = {
    {"sampled", VK_IMAGE_USAGE_SAMPLED_BIT},
    {"transfer-src", VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
    {"transfer-dst", VK_IMAGE_USAGE_TRANSFER_DST_BIT},
    {"color-attachment", VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
    {"storage", VK_IMAGE_USAGE_STORAGE_BIT},
};

static VkImageUsageFlags parse_usage(const char* csv) {
  VkImageUsageFlags out = 0;
  const char* p = csv;
  while (*p != '\0') {
    const char* comma = strchr(p, ',');
    const size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);
    bool matched = false;
    for (size_t i = 0; i < sizeof(kUsageNames) / sizeof(*kUsageNames); i++) {
      if (strlen(kUsageNames[i].name) == len &&
          strncmp(p, kUsageNames[i].name, len) == 0) {
        out |= kUsageNames[i].bit;
        matched = true;
        break;
      }
    }
    if (!matched) {
      fprintf(stderr, "unknown usage \"%.*s\"\n", (int)len, p);
      return 0;
    }
    if (comma == NULL) {
      break;
    }
    p = comma + 1;
  }
  return out;
}

static void print_usage_flags(VkImageUsageFlags usage) {
  bool first = true;
  for (size_t i = 0; i < sizeof(kUsageNames) / sizeof(*kUsageNames); i++) {
    if (usage & kUsageNames[i].bit) {
      printf("%s%s", first ? "" : ",", kUsageNames[i].name);
      first = false;
    }
  }
  if (first) {
    printf("(none)");
  }
}

struct attempt {
  VkResult alloc_rc;
  VkResult bind_rc;
  VkDeviceSize ask;
};

// One import of @fd at @ask bytes, on a dup so neither outcome disturbs the
// caller's fd.
static struct attempt try_import(struct vk* v,
                                 VkImage image,
                                 int fd,
                                 VkDeviceSize ask) {
  struct attempt a = {
      .ask = ask, .alloc_rc = VK_ERROR_UNKNOWN, .bind_rc = VK_ERROR_UNKNOWN};
  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    return a;
  }

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(v->dev, image, &req);

  PFN_vkGetMemoryFdPropertiesKHR get_fdp =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
          v->dev, "vkGetMemoryFdPropertiesKHR");
  VkMemoryFdPropertiesKHR fdp = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  get_fdp(v->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup_fd, &fdp);

  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(v->pd, &mp);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
    if ((req.memoryTypeBits & fdp.memoryTypeBits) & (1u << i)) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    close(dup_fd);
    return a;
  }

  VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup_fd};
  VkMemoryDedicatedAllocateInfo ded = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &import,
      .image = image};
  VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .pNext = &ded,
                              .allocationSize = ask,
                              .memoryTypeIndex = type};

  VkDeviceMemory mem = VK_NULL_HANDLE;
  a.alloc_rc = vkAllocateMemory(v->dev, &mai, NULL, &mem);
  if (a.alloc_rc != VK_SUCCESS) {
    close(dup_fd);  // ownership stayed with us
    return a;
  }
  // Vulkan owns dup_fd now.
  a.bind_rc = vkBindImageMemory(v->dev, image, mem, 0);
  vkFreeMemory(v->dev, mem, NULL);
  return a;
}

static int probe_one(struct vk* v,
                     struct gbm_device* gbm,
                     uint32_t fourcc,
                     VkFormat format,
                     uint64_t modifier,
                     uint32_t plane_count,
                     VkImageUsageFlags usage,
                     uint32_t w,
                     uint32_t h,
                     VkDeviceSize readahead) {
  struct gbm_bo* bo = gbm_bo_create_with_modifiers2(
      gbm, w, h, fourcc, &modifier, 1, GBM_BO_USE_RENDERING);
  if (bo == NULL) {
    printf("%5u x %-5u  gbm refused this extent with that modifier\n", w, h);
    return 0;
  }
  if (gbm_bo_get_modifier(bo) != modifier) {
    printf("%5u x %-5u  gbm gave modifier %#" PRIx64 ", asked %#" PRIx64 "\n",
           w, h, gbm_bo_get_modifier(bo), modifier);
    gbm_bo_destroy(bo);
    return 0;
  }

  int fd = gbm_bo_get_fd(bo);
  if (fd < 0) {
    printf("%5u x %-5u  gbm_bo_get_fd failed\n", w, h);
    gbm_bo_destroy(bo);
    return 0;
  }
  const off_t held = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  VkSubresourceLayout planes[MAX_PLANES] = {0};
  const uint32_t n_planes =
      plane_count > 0 ? plane_count : (uint32_t)gbm_bo_get_plane_count(bo);
  for (uint32_t i = 0; i < n_planes && i < MAX_PLANES; i++) {
    planes[i].offset = gbm_bo_get_offset(bo, (int)i);
    planes[i].rowPitch = gbm_bo_get_stride_for_plane(bo, (int)i);
  }

  VkImageDrmFormatModifierExplicitCreateInfoEXT explicit_info = {
      .sType =
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = n_planes,
      .pPlaneLayouts = planes};
  VkExternalMemoryImageCreateInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &explicit_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .pNext = &ext_info,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = format,
                           .extent = {w, h, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                           .usage = usage,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

  VkImage image = VK_NULL_HANDLE;
  VkResult r = vkCreateImage(v->dev, &ici, NULL, &image);
  if (r != VK_SUCCESS) {
    printf("%5u x %-5u  vkCreateImage: %s (%d)\n", w, h, vkstr(r), r);
    close(fd);
    gbm_bo_destroy(bo);
    return 0;
  }

  VkMemoryDedicatedRequirements ded_req = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
  VkMemoryRequirements2 req2 = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &ded_req};
  VkImageMemoryRequirementsInfo2 req_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
      .image = image};
  vkGetImageMemoryRequirements2(v->dev, &req_info, &req2);
  const VkDeviceSize need = req2.memoryRequirements.size;

  // Bytes of the dma-buf past what the image asks for. Fewer than the
  // read-ahead a driver charges the import for, and it cannot be satisfied.
  const long long spare = (long long)held - (long long)need;

  struct attempt a = try_import(v, image, fd, need);
  const bool ok = a.alloc_rc == VK_SUCCESS && a.bind_rc == VK_SUCCESS;

  printf("%5u x %-5u  stride %-7" PRIu64 "  bo %-9lld  req.size %-9" PRIu64
         "  spare %-6lld  dedicated %s  import %s",
         w, h, (uint64_t)planes[0].rowPitch, (long long)held, need, spare,
         ded_req.requiresDedicatedAllocation ? "yes" : "no ",
         ok ? "OK" : vkstr(a.alloc_rc));

  if (!ok && a.alloc_rc == VK_ERROR_INVALID_EXTERNAL_HANDLE &&
      need > readahead) {
    struct attempt b = try_import(v, image, fd, need - readahead);
    printf("  |  asked %" PRIu64 " short: %s", readahead,
           (b.alloc_rc == VK_SUCCESS && b.bind_rc == VK_SUCCESS)
               ? "OK"
               : vkstr(b.alloc_rc));
  }
  printf("\n");

  vkDestroyImage(v->dev, image, NULL);
  close(fd);
  gbm_bo_destroy(bo);
  return ok ? 0 : 1;
}

int main(int argc, char** argv) {
  const char* node = "/dev/dri/renderD128";
  uint32_t width = 1280;
  uint32_t first_height = 1;
  uint32_t max_height = 8;
  uint32_t mod_index = 0;
  VkDeviceSize readahead = 64;
  uint64_t forced_modifier = DRM_FORMAT_MOD_INVALID;
  const char* usage_csv = "color-attachment,transfer-src";

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      node = argv[++i];
    } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
      width = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--heights") == 0 && i + 1 < argc) {
      max_height = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
      first_height = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--mod-index") == 0 && i + 1 < argc) {
      mod_index = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--readahead") == 0 && i + 1 < argc) {
      readahead = (VkDeviceSize)strtoull(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--modifier") == 0 && i + 1 < argc) {
      forced_modifier = strtoull(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--usage") == 0 && i + 1 < argc) {
      usage_csv = argv[++i];
    } else {
      fprintf(stderr,
              "usage: %s [--device /dev/dri/renderDxxx] [--width N]\n"
              "          [--from N] [--heights N] [--mod-index N]\n"
              "          [--modifier 0xHEX] [--readahead N]\n"
              "          [--usage sampled,transfer-src,transfer-dst,"
              "color-attachment,storage]\n",
              argv[0]);
      return 2;
    }
  }

  int drm_fd = open(node, O_RDWR | O_CLOEXEC);
  if (drm_fd < 0) {
    fprintf(stderr, "open %s: %m\n", node);
    return 1;
  }
  struct gbm_device* gbm = gbm_create_device(drm_fd);
  if (gbm == NULL) {
    fprintf(stderr, "gbm_create_device failed on %s\n", node);
    return 1;
  }

  struct vk v = {0};
  if (!vk_init(&v)) {
    return 1;
  }

  printf("device   %s\n", v.device_name);
  printf("driver   %s (%s)\n", v.driver, v.driver_info);
  printf("gbm      %s, backend %s\n", node, gbm_device_get_backend_name(gbm));

  const uint32_t fourcc = DRM_FORMAT_XRGB8888;
  const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  const VkImageUsageFlags usage = parse_usage(usage_csv);
  if (usage == 0) {
    return 2;
  }
  printf("usage    ");
  print_usage_flags(usage);
  printf("\n");

  uint64_t mods[16];
  uint32_t mod_planes[16];
  uint32_t n = importable_modifiers(&v, format, usage, mods, mod_planes, 16);
  if (forced_modifier != DRM_FORMAT_MOD_INVALID) {
    mods[0] = forced_modifier;
    mod_planes[0] = 1;
    n = 1;
    printf("modifier %#" PRIx64 " (forced)\n", mods[0]);
  } else if (n == 0) {
    fprintf(stderr,
            "driver lists no importable modifier for XR24 with this usage\n");
    return 1;
  } else {
    if (mod_index >= n) {
      fprintf(stderr, "--mod-index %u but only %u are importable\n", mod_index,
              n);
      return 1;
    }
    printf("modifier %#" PRIx64 ", %u of %u the driver will import for XR24\n",
           mods[mod_index], mod_index, n);
    mods[0] = mods[mod_index];
    mod_planes[0] = mod_planes[mod_index];
  }
  printf("\n");

  int refused = 0;
  uint32_t tried = 0;
  for (uint32_t h = first_height; h <= max_height; h++, tried++) {
    refused += probe_one(&v, gbm, fourcc, format, mods[0], mod_planes[0], usage,
                         width, h, readahead);
  }

  printf(
      "\n%d of %u extents refused an import of a dma-buf this same stack "
      "allocated for them.\n",
      refused, tried);
  if (refused > 0) {
    printf(
        "Read the spare column: the refusals are the extents leaving under "
        "%" PRIu64
        " bytes in\nthe image's last page, and each imports when "
        "asked %" PRIu64 " bytes short.\n",
        readahead, readahead);
  }
  return refused > 0 ? 1 : 0;
}
