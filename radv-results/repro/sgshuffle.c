/* sgshuffle.c: sgprobe.c with sgshuffle.spv: do subgroup shuffles reach across 64 lanes?
 * One 256-wide workgroup; prints the device's subgroup properties, then the
 * distinct values of each builtin across the workgroup. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n", #x, r_); exit(2); } } while (0)
static void distinct(const char *name, const uint32_t *v, int col) {
    uint32_t seen[256]; int ns = 0; uint32_t mx = 0;
    for (int i = 0; i < 256; i++) {
        uint32_t x = v[i * 5 + col]; int k = 0; if (x > mx) mx = x;
        while (k < ns && seen[k] != x) k++;
        if (k == ns && ns < 256) seen[ns++] = x;
    }
    printf("%-22s", name);
    for (int k = 0; k < ns && k < 16; k++) printf(" %u", seen[k]);
    printf(ns > 16 ? " ... (%d distinct) max=%u\n" : "  (%d distinct) max=%u\n", ns, mx);
}
int main(void) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "sgprobe", 1, NULL, 0, VK_API_VERSION_1_2};
    uint32_t nie = 0; vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *iep = calloc(nie ? nie : 1, sizeof *iep); vkEnumerateInstanceExtensionProperties(NULL, &nie, iep);
    const char *ie[1]; uint32_t nie_on = 0; VkInstanceCreateFlags iflags = 0;
    for (uint32_t i = 0; i < nie; i++)
        if (!strcmp(iep[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            ie[nie_on++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME; iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, iflags, &app, 0, NULL, nie_on, ie};
    VkInstance inst; CK(vkCreateInstance(&ii, NULL, &inst));
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
    VkPhysicalDeviceSubgroupProperties sp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &sp};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    printf("device %s | driver %s %s | subgroupSize %u | ops 0x%x | stages 0x%x\n", p2.properties.deviceName,
           dp.driverName, dp.driverInfo, sp.subgroupSize, sp.supportedOperations, sp.supportedStages);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep); vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep);
    const char *de[2]; uint32_t nde = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset")) de[nde++] = "VK_KHR_portability_subset";
    /* SGPROBE_VARY=1: opt the pipeline into Metal's own simdgroup width
     * (VK_EXT_subgroup_size_control, ALLOW_VARYING_SUBGROUP_SIZE). */
    int vary = getenv("SGPROBE_VARY") ? atoi(getenv("SGPROBE_VARY")) : 0;
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT scf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT,
                                                          .subgroupSizeControl = VK_TRUE, .computeFullSubgroups = VK_TRUE};
    if (vary) {
        int has = 0;
        for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME)) has = 1;
        if (!has) { printf("sgprobe: VK_EXT_subgroup_size_control not offered\n"); return 2; }
        de[nde++] = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    }
    printf("sgprobe: allow varying subgroup size %s\n", vary > 1 ? "on + require full subgroups" : vary ? "on" : "off");
    float qp = 1.f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &qp};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, vary ? &scf : NULL, 0, 1, &qi, 0, NULL, nde, de, NULL};
    VkDevice dev; CK(vkCreateDevice(phys, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    printf("queue family 0 used\n");
    VkDeviceSize sz = 256 * 5 * 4;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf; CK(vkCreateBuffer(dev, &bi, NULL, &buf));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, buf, &rq);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mt = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rq.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { mt = i; break; }
    if (mt == UINT32_MAX) { printf("no host-visible coherent memory\n"); return 2; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, mt};
    VkDeviceMemory mem; CK(vkAllocateMemory(dev, &ai, NULL, &mem)); CK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t *v; CK(vkMapMemory(dev, mem, 0, sz, 0, (void **)&v)); memset(v, 0xff, (size_t)sz);
    FILE *f = fopen("sgshuffle.spv", "rb"); if (!f) { printf("sgshuffle.spv missing\n"); return 2; }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)fl); if (fread(code, 1, (size_t)fl, f) != (size_t)fl) return 2; fclose(f);
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)fl, code};
    VkShaderModule sh; CK(vkCreateShaderModule(dev, &si, NULL, &sh));
    VkDescriptorSetLayoutBinding lb = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &lb};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, vary ? (VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT | (vary > 1 ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT : 0)) : 0, VK_SHADER_STAGE_COMPUTE_BIT, sh, "main", NULL}, pl, VK_NULL_HANDLE, 0};
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool pool; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &da, &ds));
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cba, &cb));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    CK(vkBeginCommandBuffer(cb, &bbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    CK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    distinct("gl_SubgroupSize", v, 0);
    distinct("shuffleXor(lane, 32) partner right", v, 1);
    distinct("butterfly over masks 1..16 (32 if right)", v, 2);
    distinct("butterfly over masks 1..32 (64 if right)", v, 3);
    distinct("subgroupAdd(1)", v, 4);
    uint32_t sz0 = v[0], add0 = v[4];
    printf("sgprobe: reported %u, counted %u -> %s\n", sz0, add0, sz0 == add0 ? "CONSISTENT" : "INCONSISTENT");
    return sz0 == add0 ? 0 : 1;
}
