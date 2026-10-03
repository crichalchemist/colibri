/* One compute shader, two pipelines built from it: one whose layout has plain
 * STORAGE_BUFFER bindings (never dispatched) and one whose layout makes the same
 * bindings STORAGE_BUFFER_DYNAMIC. The dynamic pipeline runs twice, at dynamic
 * offsets 0 and W, with one descriptor set; each run must fill its own window.
 *
 *   argv[1]  0: both pipelines from one VkShaderModule   1: each from its own module
 *   argv[2]  0: static pipeline created first             1: dynamic pipeline first
 *
 * Build: glslc --target-env=vulkan1.2 copy.comp -o copy.spv
 *        cc -O2 -Wall -Wextra mvk_shared_module.c -o mvk_shared_module -lvulkan
 * Exit status 0 = both windows right, 1 = wrong, 2 = setup failure. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n", #x, r_); exit(2); } } while (0)
#define N 64

static VkDevice dev;
static uint32_t *code; static size_t code_size;

static VkShaderModule module(void) {
    VkShaderModuleCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code_size, .pCode = code};
    VkShaderModule m; CK(vkCreateShaderModule(dev, &si, NULL, &m)); return m;
}
static VkPipeline pipeline(VkShaderModule m, VkDescriptorType t, VkPipelineLayout *pl) {
    VkDescriptorSetLayoutBinding b[2] = {{0, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    CK(vkCreatePipelineLayout(dev, &pli, NULL, pl));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = m, .pName = "main"}, .layout = *pl};
    VkPipeline p; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p)); return p;
}

int main(int argc, char **argv) {
    int own = argc > 1 ? atoi(argv[1]) : 0, dyn_first = argc > 2 ? atoi(argv[2]) : 0;

    /* instance: portability enumeration only where the loader offers it (MoltenVK) */
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *iep = calloc(nie ? nie : 1, sizeof *iep); vkEnumerateInstanceExtensionProperties(NULL, &nie, iep);
    const char *ie[1]; uint32_t nie_on = 0; VkInstanceCreateFlags ifl = 0;
    for (uint32_t i = 0; i < nie; i++)
        if (!strcmp(iep[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            ie[nie_on++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME; ifl |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "mvk_shared_module",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = ifl, .pApplicationInfo = &app,
                               .enabledExtensionCount = nie_on, .ppEnabledExtensionNames = ie};
    VkInstance inst; CK(vkCreateInstance(&ii, NULL, &inst));

    /* device: discrete over integrated over anything else, CPU last */
    uint32_t nd = 0; CK(vkEnumeratePhysicalDevices(inst, &nd, NULL));
    VkPhysicalDevice *pds = calloc(nd ? nd : 1, sizeof *pds); CK(vkEnumeratePhysicalDevices(inst, &nd, pds));
    int best = -1, rank = -1;
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        int r = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
        if (r > rank) { rank = r; best = (int)i; }
    }
    if (best < 0) { printf("no Vulkan device\n"); return 2; }
    VkPhysicalDevice phys = pds[best];
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    printf("device %s | driver %s %s\n", p2.properties.deviceName, drv.driverName, drv.driverInfo);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep); vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep);
    const char *de[1]; uint32_t nde = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset")) de[nde++] = "VK_KHR_portability_subset";
    float qp = 1.f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1,
                                  .pQueuePriorities = &qp};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = nde, .ppEnabledExtensionNames = de};
    CK(vkCreateDevice(phys, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    /* two windows of N floats in each buffer, W bytes apart */
    VkDeviceSize al = p2.properties.limits.minStorageBufferOffsetAlignment, W = (N * 4 + al - 1) / al * al;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    VkBuffer buf[2]; VkDeviceMemory mem[2]; float *ptr[2];
    for (int k = 0; k < 2; k++) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * W,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        CK(vkCreateBuffer(dev, &bi, NULL, &buf[k]));
        VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf[k], &rq);
        uint32_t mt = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
            if ((rq.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) mt = i;
        if (mt == UINT32_MAX) { printf("no host-visible coherent memory\n"); return 2; }
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = mt};
        CK(vkAllocateMemory(dev, &ai, NULL, &mem[k])); CK(vkBindBufferMemory(dev, buf[k], mem[k], 0));
        CK(vkMapMemory(dev, mem[k], 0, 2 * W, 0, (void **)&ptr[k]));
    }
    float *src = ptr[0], *dst = ptr[1];
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < N; i++) { src[w * W / 4 + i] = (float)(1000 * w + i); dst[w * W / 4 + i] = -1.f; }

    FILE *f = fopen("copy.spv", "rb"); if (!f) { printf("copy.spv missing\n"); return 2; }
    fseek(f, 0, SEEK_END); code_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    code = malloc(code_size); if (fread(code, 1, code_size, f) != code_size) return 2; fclose(f);

    /* the two pipelines, in the requested order and module arrangement */
    VkShaderModule m0 = module(), m1 = own ? module() : m0;
    VkPipelineLayout pl_static, pl_dyn; VkPipeline p_static, p_dyn;
    if (dyn_first) { p_dyn = pipeline(m1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, &pl_dyn);
                     p_static = pipeline(m0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pl_static); }
    else           { p_static = pipeline(m0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pl_static);
                     p_dyn = pipeline(m1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, &pl_dyn); }
    (void)p_static;   /* created, never dispatched */

    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 2};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo da = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &da, &ds));
    VkDescriptorBufferInfo bi[2] = {{buf[0], 0, N * 4}, {buf[1], 0, N * 4}};
    VkWriteDescriptorSet wr[2];
    for (int k = 0; k < 2; k++)
        wr[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)k,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .pBufferInfo = &bi[k]};
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);

    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cba, &cb));
    VkCommandBufferBeginInfo bbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb, &bbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p_dyn);
    for (int w = 0; w < 2; w++) {
        uint32_t off[2] = {(uint32_t)(w * W), (uint32_t)(w * W)};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl_dyn, 0, 1, &ds, 2, off);
        struct { uint32_t n; float tag; } pc = {N, (float)(10 * w + 7)};
        vkCmdPushConstants(cb, pl_dyn, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pc);
        vkCmdDispatch(cb, 1, 1, 1);
    }
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));

    int bad = 0;
    for (int w = 0; w < 2; w++) {
        int wrong = 0;
        for (int i = 0; i < N; i++) wrong += dst[w * W / 4 + i] != (float)(1000 * w + i) + (float)(10 * w + 7);
        printf("window %d: dst[0]=%g want %g, %d of %d wrong\n", w, dst[w * W / 4], (double)(1000 * w + 10 * w + 7), wrong, N);
        bad |= wrong != 0;
    }
    printf("mvk_shared_module: module=%s order=%s W=%llu -> %s\n", own ? "own" : "shared", dyn_first ? "dynamic-first" : "static-first",
           (unsigned long long)W, bad ? "FAIL" : "PASS");
    return bad;
}
