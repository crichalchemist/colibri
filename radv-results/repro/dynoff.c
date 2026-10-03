/* dynoff.c: K "experts", each a window of N floats at a dynamic offset in shared
 * x/h/y buffers; stage 0 x->h, barrier, stage 1 h->y; read y back and check
 * y = 2*x + tag + 1 per expert. argv: K N private(0|1) static(0|1) gap(0|1|4) twin(0|1|2). */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n", #x, r_); exit(2); } } while (0)
static VkDevice dev; static VkPhysicalDevice phys;
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want && !(mp.memoryTypes[i].propertyFlags & avoid)) return i;
    printf("no memory type\n"); exit(2);
}
static void mkbuf(VkDeviceSize sz, VkBufferUsageFlags use, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid,
                  VkBuffer *b, VkDeviceMemory *m, void **p) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    CK(vkCreateBuffer(dev, &bi, NULL, b));
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(dev, *b, &rq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rq.size, memtype(rq.memoryTypeBits, want, avoid)};
    CK(vkAllocateMemory(dev, &ai, NULL, m)); CK(vkBindBufferMemory(dev, *b, *m, 0));
    if (p) CK(vkMapMemory(dev, *m, 0, sz, 0, p));
}
int main(int argc, char **argv) {
    int K = argc > 1 ? atoi(argv[1]) : 4, N = argc > 2 ? atoi(argv[2]) : 160;
    int priv = argc > 3 ? atoi(argv[3]) : 1, stat = argc > 4 ? atoi(argv[4]) : 0;
    int gap = argc > 5 ? atoi(argv[5]) : 0;
    int twin = argc > 6 ? atoi(argv[6]) : 0;
    if (twin && stat) { printf("twin ignored\n"); twin = 0; }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dynoff", 1, NULL, 0, VK_API_VERSION_1_2};
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
    phys = pds[best];
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(phys, &pr);
    printf("device %s\n", pr.deviceName);
    VkDeviceSize al = pr.limits.minStorageBufferOffsetAlignment < 16 ? 16 : pr.limits.minStorageBufferOffsetAlignment;
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep); vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep);
    const char *de[1]; uint32_t nde = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset")) de[nde++] = "VK_KHR_portability_subset";
    float qp = 1.f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &qp};
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qi, 0, NULL, nde, de, NULL};
    CK(vkCreateDevice(phys, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
    printf("queue family 0 used\n");
    VkDeviceSize win = ((VkDeviceSize)N * 4 + al - 1) / al * al, region = win * K;
    VkBuffer bx, bh, by; VkDeviceMemory mx, mh, my; float *px, *py, *ph = NULL;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    mkbuf(2 * region, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, 0, &bx, &mx, (void **)&px);
    if (priv) mkbuf(2 * region, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &bh, &mh, NULL);
    else mkbuf(2 * region, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, 0, &bh, &mh, (void **)&ph);
    if (!priv) for (VkDeviceSize e = 0; e < 2 * region / 4; e++) ph[e] = -5.0f;
    VkBuffer bs = VK_NULL_HANDLE; VkDeviceMemory ms; float *ps_ = NULL;
    if (gap) {
        mkbuf(2 * region, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, 0, &bs, &ms, (void **)&ps_);
        for (VkDeviceSize e = 0; e < 2 * region / 4; e++) ps_[e] = e < (VkDeviceSize)N ? 1.0f : 3.0f;
    }
    mkbuf(2 * region, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv, 0, &by, &my, (void **)&py);
    for (int k = 0; k < K; k++) for (int i = 0; i < N; i++) px[k * win / 4 + i] = (float)(k * 1000 + i);
    { float *py0 = py; for (VkDeviceSize e = 0; e < 2 * region / 4; e++) py0[e] = -7.0f; }
    char spv[64]; if (gap) snprintf(spv, sizeof spv, "dynoff_gap%d.spv", gap); else snprintf(spv, sizeof spv, "dynoff.spv");
    FILE *f = fopen(spv, "rb"); if (!f) { printf("%s missing\n", spv); return 2; }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)fl); if (fread(code, 1, (size_t)fl, f) != (size_t)fl) return 2; fclose(f);
    VkShaderModuleCreateInfo si = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)fl, code};
    VkShaderModule sh; CK(vkCreateShaderModule(dev, &si, NULL, &sh));
    VkDescriptorType dt = stat ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    int nb = gap + 2;
    VkDescriptorSetLayoutBinding lb[8];
    for (int i = 0; i < nb; i++) {
        VkDescriptorType t = (i == 0 || i == nb - 1) ? dt : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        VkDescriptorSetLayoutBinding one = {(uint32_t)i, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; lb[i] = one;
    }
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nb, lb};
    VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sh, "main", NULL}, pl, VK_NULL_HANDLE, 0};
    if (twin) {   /* unused static twin built from the same module sh, all bindings STORAGE_BUFFER */
        VkDescriptorSetLayoutBinding tb[8];
        for (int i = 0; i < nb; i++) { VkDescriptorSetLayoutBinding one = {(uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; tb[i] = one; }
        VkDescriptorSetLayoutCreateInfo tli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nb, tb};
        VkDescriptorSetLayout tdsl; CK(vkCreateDescriptorSetLayout(dev, &tli, NULL, &tdsl));
        VkPipelineLayoutCreateInfo tpli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &tdsl, 1, &pcr};
        VkPipelineLayout tpl; CK(vkCreatePipelineLayout(dev, &tpli, NULL, &tpl));
        VkComputePipelineCreateInfo tcpi = cpi; tcpi.layout = tpl;
        VkPipeline tpipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &tcpi, NULL, &tpipe));
        if (twin == 2) {   /* control: the dynamic pipeline gets a second module from the same words */
            VkShaderModule sh2; CK(vkCreateShaderModule(dev, &si, NULL, &sh2));
            cpi.stage.module = sh2;
        }
    }
    VkPipeline pipe; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    int nsets = stat ? 2 * K : 2;
    VkDescriptorPoolSize ps[2] = {{dt, (uint32_t)(2 * nsets)}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(gap * nsets)}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, (uint32_t)nsets, gap ? 2u : 1u, ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSet *ds = calloc((size_t)nsets, sizeof *ds);
    for (int s = 0; s < nsets; s++) { VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl}; CK(vkAllocateDescriptorSets(dev, &da, &ds[s])); }
    /* set (stage s, expert k): dynamic -> ds[s] with region windows; static -> ds[2k+s] with the offset inside */
    for (int s = 0; s < nsets; s++) {
        int stg = s % 2, k = s / 2; VkDeviceSize o = stat ? (VkDeviceSize)k * win : 0, w = stat ? (VkDeviceSize)N * 4 : region;
        VkDescriptorBufferInfo b[8]; VkWriteDescriptorSet wr[8];
        for (int i = 0; i < nb; i++) {
            VkDescriptorBufferInfo bi_ = i == 0 ? (VkDescriptorBufferInfo){stg ? bh : bx, o, w}
                : i == nb - 1 ? (VkDescriptorBufferInfo){stg ? by : bh, o, w} : (VkDescriptorBufferInfo){bs, 0, 2 * region};
            b[i] = bi_;
            VkDescriptorType t = (i == 0 || i == nb - 1) ? dt : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            VkWriteDescriptorSet w_ = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds[s], (uint32_t)i, 0, 1, t, NULL, &b[i], NULL};
            wr[i] = w_;
        }
        vkUpdateDescriptorSets(dev, (uint32_t)nb, wr, 0, NULL);
    }
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cba = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cba, &cb));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    CK(vkBeginCommandBuffer(cb, &bbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    for (int stg = 0; stg < 2; stg++) {
        for (int k = 0; k < K; k++) {
            uint32_t dyn[2] = {(uint32_t)(k * win), (uint32_t)(k * win)};
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, stat ? &ds[2 * k + stg] : &ds[stg], stat ? 0 : 2, stat ? NULL : dyn);
            int pc[3] = {N, stg, k};
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, pc);
            vkCmdDispatch(cb, (uint32_t)((N + 63) / 64), 1, 1);
        }
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                              stg ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_SHADER_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             stg ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo sub = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    CK(vkQueueSubmit(q, 1, &sub, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    int bad = 0;
    for (int k = 0; k < K; k++) {
        int kb = 0;
        for (int i = 0; i < N; i++) { float want = 2.f * (float)(k * 1000 + i) + (float)k + 1.f; if (py[k * win / 4 + i] != want) kb++; }
        printf("expert %d: %d of %d wrong\n", k, kb, N); bad |= kb != 0;
        float y0 = py[k * win / 4], w0 = 2.f * (float)(k * 1000) + (float)k + 1.f;
        printf("window %d: y0=%g want %g", k, y0, w0);
        if (y0 == w0) printf(" ok\n");
        else if (y0 == -7.f) printf(" untouched\n");
        else printf(" <- x window %d, tag %d (raw y0=%g)\n", (int)(y0 - 1) / 2000, (int)(y0 - 1) % 2000, y0);
        if (!priv) {
            float h0 = ph[k * win / 4], hw = 2.f * (float)(k * 1000) + (float)k;
            printf("h window %d: h0=%g want %g%s\n", k, h0, hw, h0 == hw ? " ok" : h0 == -5.f ? " untouched" : "");
        }
    }
    printf("dynoff K=%d N=%d h=%s offsets=%s gap=%d twin=%d align=%llu -> %s\n", K, N, priv ? "device-local" : "host-visible",
           stat ? "static" : "dynamic", gap, twin, (unsigned long long)al, bad ? "FAIL" : "PASS");
    return bad;
}
