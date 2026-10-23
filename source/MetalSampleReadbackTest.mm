// SPDX-License-Identifier: GPL-2.0-only
#include "emulator/src/Graphics/Metal/MetalVulkanInternal.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef LUMEN_SAMPLE_READBACK_SHADER_PATH
#define LUMEN_SAMPLE_READBACK_SHADER_PATH "source/gen4/src/video_core/host_shaders/sample_readback.comp"
#endif
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
#define OK(x) CHECK((x)==VK_SUCCESS)

static std::vector<uint32_t> CompileSource(const std::string& source, EShLanguage stage,
                                          const std::string& defines = {}) {
    const char* text = source.c_str();
    glslang::TShader shader(stage);
    shader.setStrings(&text,1);
    shader.setPreamble(defines.c_str());
    shader.setEnvInput(glslang::EShSourceGlsl,stage,glslang::EShClientVulkan,450);
    shader.setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_3);
    const auto messages=EShMessages(EShMsgSpvRules|EShMsgVulkanRules);
    if(!shader.parse(GetDefaultResources(),450,false,messages)) { std::fprintf(stderr,"%s\n",shader.getInfoLog()); std::abort(); }
    glslang::TProgram program; program.addShader(&shader); CHECK(program.link(messages));
    std::vector<uint32_t> words; glslang::GlslangToSpv(*program.getIntermediate(stage),words);
    return words;
}
static std::vector<uint32_t> CompileReadback(bool depth, bool stencil) {
    std::ifstream file(LUMEN_SAMPLE_READBACK_SHADER_PATH); CHECK(file.good());
    const std::string source{std::istreambuf_iterator<char>(file), {}};
    const std::string defines = std::string("#define FLOAT_INPUT ")+(depth?"1":"0")+
        "\n#define DEPTH_BITS 32\n#define CHANNEL_BITS "+(stencil?"8":"32")+
        "\n#define CHANNELS 1\n#define TEXEL_BYTES "+(stencil?"1":"4")+"\n#define PACKED_10 0\n";
    return CompileSource(source, EShLangCompute, defines);
}

static void CheckImmutableSamplers(VkDevice device, VkQueue queue) {
    std::array<VkSampler,2> samplers{};
    for (size_t i=0;i<samplers.size();++i) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.minFilter=info.magFilter=i?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        info.addressModeU=info.addressModeV=info.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        OK(vkCreateSampler(device,&info,nullptr,&samplers[i]));
    }
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType=VK_IMAGE_TYPE_2D;imageInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent={2,1,1};imageInfo.mipLevels=imageInfo.arrayLayers=1;
    imageInfo.samples=VK_SAMPLE_COUNT_1_BIT;imageInfo.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image{};OK(vkCreateImage(device,&imageInfo,nullptr,&image));
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
    VkDeviceMemory imageMemory{};OK(vkAllocateMemory(device,&allocation,nullptr,&imageMemory));
    OK(vkBindImageMemory(device,image,imageMemory,0));
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image=image;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=imageInfo.format;
    viewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};OK(vkCreateImageView(device,&viewInfo,nullptr,&view));
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bufferInfo.size=4096;
    bufferInfo.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer{};OK(vkCreateBuffer(device,&bufferInfo,nullptr,&buffer));
    vkGetBufferMemoryRequirements(device,buffer,&requirements);
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=1;
    VkDeviceMemory memory{};OK(vkAllocateMemory(device,&allocation,nullptr,&memory));
    OK(vkBindBufferMemory(device,buffer,memory,0));
    void* mapped{};OK(vkMapMemory(device,memory,0,4096,0,&mapped));
    const std::array<uint32_t,2> pixels{0xFF000000,0xFF0000FF};
    std::memcpy(mapped,pixels.data(),sizeof(pixels));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commands{};OK(vkCreateCommandPool(device,&poolInfo,nullptr,&commands));
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool=commands;commandInfo.commandBufferCount=1;
    VkCommandBuffer command{};OK(vkAllocateCommandBuffers(device,&commandInfo,&command));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    OK(vkBeginCommandBuffer(command,&begin));
    VkBufferImageCopy upload{};upload.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};upload.imageExtent={2,1,1};
    vkCmdCopyBufferToImage(command,buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload);
    OK(vkEndCommandBuffer(command));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    OK(vkQueueSubmit(queue,1,&submit,{}));OK(vkQueueWaitIdle(queue));

    // Both blocks occupy 120 Vulkan bytes and 128 Metal bytes. The head case
    // updates only eight bytes; the unused declared tail still needs backing.
    for (bool tailBias : {false,true}) for (bool combined : {false,true}) {
        constexpr uint32_t pushBytes=120;
        const uint32_t biasOffset=tailBias?112:0;
        const std::string pushDeclaration=tailBias ?
            "layout(push_constant) uniform P{vec4 padding[7];float bias0;float bias1;};" :
            "layout(push_constant) uniform P{float bias0;float bias1;vec4 padding[6];vec2 tail;};";
        const std::string shaderText=combined ?
            std::string("#version 450\nlayout(local_size_x=1) in;layout(set=0,binding=0) uniform sampler2D tex[2];"
            "layout(set=0,binding=2,std430) buffer Output{float result[2];};")+pushDeclaration+
            "void main(){result[0]=textureLod(tex[0],vec2(0.375,0.5),0).r+bias0;result[1]=textureLod(tex[1],vec2(0.375,0.5),0).r+bias1;}" :
            std::string("#version 450\nlayout(local_size_x=1) in;layout(set=0,binding=0) uniform texture2D tex;"
            "layout(set=0,binding=1) uniform sampler filters[2];layout(set=0,binding=2,std430) buffer Output{float result[2];};")+pushDeclaration+
            "void main(){result[0]=textureLod(sampler2D(tex,filters[0]),vec2(0.375,0.5),0).r+bias0;"
            "result[1]=textureLod(sampler2D(tex,filters[1]),vec2(0.375,0.5),0).r+bias1;}";
        const auto words=CompileSource(shaderText,EShLangCompute);
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize=words.size()*sizeof(uint32_t);moduleInfo.pCode=words.data();
        VkShaderModule module{};OK(vkCreateShaderModule(device,&moduleInfo,nullptr,&module));
        const uint32_t samplerBinding=combined?0:1;
        const VkDescriptorType samplerType=combined?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:VK_DESCRIPTOR_TYPE_SAMPLER;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        if (!combined) bindings.push_back({0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        bindings.push_back({samplerBinding,samplerType,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        bindings.push_back({2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        std::array<VkDescriptorSetLayout,2> layouts{};
        auto callerSamplers=samplers;
        for (size_t i=0;i<layouts.size();++i) {
            bindings[combined?0:1].pImmutableSamplers=i?nullptr:callerSamplers.data();
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            info.bindingCount=bindings.size();info.pBindings=bindings.data();
            OK(vkCreateDescriptorSetLayout(device,&info,nullptr,&layouts[i]));
        }
        // Caller storage may change as soon as layout creation has returned.
        callerSamplers={samplers[1],samplers[0]};
        const std::array<VkDescriptorPoolSize,3> sizes{{{samplerType,4},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}}};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets=2;descriptorInfo.poolSizeCount=sizes.size();descriptorInfo.pPoolSizes=sizes.data();
        VkDescriptorPool descriptors{};OK(vkCreateDescriptorPool(device,&descriptorInfo,nullptr,&descriptors));
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool=descriptors;setInfo.descriptorSetCount=2;setInfo.pSetLayouts=layouts.data();
        std::array<VkDescriptorSet,2> sets{};OK(vkAllocateDescriptorSets(device,&setInfo,sets.data()));
        std::array<VkPipelineLayout,2> pipelineLayouts{};
        std::array<VkPipeline,2> pipelines{};
        const std::array<VkDescriptorImageInfo,2> imageInfos{{{samplers[1],view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{samplers[0],view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
        VkDescriptorBufferInfo bufferDescriptor{buffer,256,2*sizeof(float)};
        for(size_t i=0;i<sets.size();++i) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet=sets[i];write.dstBinding=2;write.descriptorCount=1;
            write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&bufferDescriptor;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            write.pBufferInfo=nullptr;write.pImageInfo=imageInfos.data();
            if (!combined) {
                write.dstBinding=0;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            }
            if(combined || i==1) {
                write.dstBinding=samplerBinding;write.descriptorType=samplerType;write.descriptorCount=2;
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            }
            VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&layouts[i];
            const VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT,0,pushBytes};
            layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&constants;
            OK(vkCreatePipelineLayout(device,&layoutInfo,nullptr,&pipelineLayouts[i]));
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.layout=pipelineLayouts[i];
            pipelineInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
            OK(vkCreateComputePipelines(device,{},1,&pipelineInfo,nullptr,&pipelines[i]));
        }
        auto check=[&](size_t index,float first,float second) {
            std::memset(static_cast<uint8_t*>(mapped)+256,0xA5,2*sizeof(float));
            OK(vkResetCommandBuffer(command,0));OK(vkBeginCommandBuffer(command,&begin));
            // Partial push constants precede pipeline and descriptor binding;
            // caller bytes may change immediately after each recorded update.
            float value=0.125f;
            vkCmdPushConstants(command,pipelineLayouts[index],VK_SHADER_STAGE_COMPUTE_BIT,biasOffset,sizeof(value),&value);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[index]);
            value=0.25f;
            vkCmdPushConstants(command,pipelineLayouts[index],VK_SHADER_STAGE_COMPUTE_BIT,biasOffset+sizeof(value),sizeof(value),&value);
            value=-100.0f;
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayouts[index],0,1,&sets[index],0,nullptr);
            vkCmdDispatch(command,1,1,1);OK(vkEndCommandBuffer(command));
            OK(vkQueueSubmit(queue,1,&submit,{}));OK(vkQueueWaitIdle(queue));
            const auto* result=reinterpret_cast<const float*>(static_cast<uint8_t*>(mapped)+256);
            CHECK(std::abs(result[0]-(first+0.125f))<0.001f && std::abs(result[1]-(second+0.25f))<0.001f);
        };
        check(0,0.0f,0.25f);check(1,0.25f,0.0f);
        VkCopyDescriptorSet copy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
        copy.srcSet=sets[0];copy.dstSet=sets[1];copy.srcBinding=copy.dstBinding=samplerBinding;copy.descriptorCount=2;
        vkUpdateDescriptorSets(device,0,nullptr,1,&copy);check(1,0.0f,0.25f);
        // Copying from immutable into mutable descriptors must not freeze them.
        VkWriteDescriptorSet rewrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        rewrite.dstSet=sets[1];rewrite.dstBinding=samplerBinding;rewrite.descriptorType=samplerType;
        rewrite.descriptorCount=2;rewrite.pImageInfo=imageInfos.data();
        vkUpdateDescriptorSets(device,1,&rewrite,0,nullptr);check(1,0.25f,0.0f);
        if(combined) {
            copy.srcSet=sets[1];copy.dstSet=sets[0];
            vkUpdateDescriptorSets(device,0,nullptr,1,&copy);check(0,0.0f,0.25f);
        }
        for(size_t i=0;i<sets.size();++i) {vkDestroyPipeline(device,pipelines[i],nullptr);vkDestroyPipelineLayout(device,pipelineLayouts[i],nullptr);}
        vkDestroyDescriptorPool(device,descriptors,nullptr);
        for(auto layout:layouts) vkDestroyDescriptorSetLayout(device,layout,nullptr);
        vkDestroyShaderModule(device,module,nullptr);
    }
    vkDestroyCommandPool(device,commands,nullptr);
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);
    vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,imageMemory,nullptr);
    for(auto sampler:samplers) vkDestroySampler(device,sampler,nullptr);
    std::puts("native Metal immutable sampler arrays, separate/combined sampling, updates and copies: PASS");
}

int main() { @autoreleasepool {
    CHECK(glslang::InitializeProcess());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; VkInstance instance{};
    OK(vkCreateInstance(&ici,nullptr,&instance)); uint32_t count=1; VkPhysicalDevice physical{};
    OK(vkEnumeratePhysicalDevices(instance,&count,&physical));
    const float priority=1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount=1; qci.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&features12};
    vkGetPhysicalDeviceFeatures2(physical,&features);CHECK(features12.drawIndirectCount);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&features12};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    VkDevice device{};OK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue{};vkGetDeviceQueue(device,0,0,&queue);
    CheckImmutableSamplers(device,queue);
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache pipelineCache{};OK(vkCreatePipelineCache(device,&cacheInfo,nullptr,&pipelineCache));
    std::array<VkImage,2> images{}; std::array<VkDeviceMemory,2> imageMemory{};
    for(size_t i=0;i<2;++i) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.imageType=VK_IMAGE_TYPE_2D;ci.format=i?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent={4,4,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_4_BIT;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|(i?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        OK(vkCreateImage(device,&ci,nullptr,&images[i]));VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,images[i],&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;
        OK(vkAllocateMemory(device,&ai,nullptr,&imageMemory[i]));OK(vkBindImageMemory(device,images[i],imageMemory[i],0));
    }
    // Independent native fixture writes four different color/depth/stencil
    // samples. The production GLSL reader must preserve them individually.
    NSString* fixture=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}\n"
        "struct O {float4 c [[color(0)]];float d [[depth(any)]];uint mask [[sample_mask]];};\n"
        "fragment O f(uint sample [[sample_id]],constant uint& selected [[buffer(0)]]) {O o;o.c=float4(17+sample,33+sample,65+sample,255)/255.0;o.d=float(sample+1)/8.0;o.mask=1u<<selected;return o;}";
    NSError* error=nil;auto lib=[device->metalDevice newLibraryWithSource:fixture options:nil error:&error];
    if(lib==nil)std::fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);CHECK(lib!=nil);
    auto pd=[[MTLRenderPipelineDescriptor alloc]init];pd.vertexFunction=[lib newFunctionWithName:@"v"];pd.fragmentFunction=[lib newFunctionWithName:@"f"];
    pd.rasterSampleCount=4;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    auto pipeline=[device->metalDevice newRenderPipelineStateWithDescriptor:pd error:&error];CHECK(pipeline!=nil);
    auto ds=[[MTLDepthStencilDescriptor alloc]init];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;
    auto stencil=[[MTLStencilDescriptor alloc]init];stencil.stencilCompareFunction=MTLCompareFunctionAlways;stencil.depthStencilPassOperation=MTLStencilOperationReplace;
    ds.frontFaceStencil=ds.backFaceStencil=stencil;auto depthState=[device->metalDevice newDepthStencilStateWithDescriptor:ds];
    auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=images[0]->metalTexture;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=pass.stencilAttachment.texture=images[1]->metalTexture;
    pass.depthAttachment.loadAction=pass.stencilAttachment.loadAction=MTLLoadActionClear;
    pass.depthAttachment.storeAction=pass.stencilAttachment.storeAction=MTLStoreActionStore;
    auto native=[queue->metalQueue commandBuffer];auto enc=[native renderCommandEncoderWithDescriptor:pass];CHECK(enc!=nil);
    [enc setRenderPipelineState:pipeline];[enc setDepthStencilState:depthState];
    for(uint32_t i=0;i<4;++i){[enc setStencilReferenceValue:17+i];[enc setFragmentBytes:&i length:4 atIndex:0];[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];}
    [enc endEncoding];[native commit];[native waitUntilCompleted];CHECK(native.status==MTLCommandBufferStatusCompleted);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=4096;bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VkBuffer buffer{};OK(vkCreateBuffer(device,&bci,nullptr,&buffer));VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};bai.allocationSize=4096;bai.memoryTypeIndex=1;
    VkDeviceMemory outputMemory{};OK(vkAllocateMemory(device,&bai,nullptr,&outputMemory));OK(vkBindBufferMemory(device,buffer,outputMemory,0));
    const std::array<VkDescriptorSetLayoutBinding,2> bindings{{{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};lci.bindingCount=2;lci.pBindings=bindings.data();VkDescriptorSetLayout setLayout{};OK(vkCreateDescriptorSetLayout(device,&lci,nullptr,&setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,36};VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};plci.setLayoutCount=1;plci.pSetLayouts=&setLayout;plci.pushConstantRangeCount=1;plci.pPushConstantRanges=&push;VkPipelineLayout layout{};OK(vkCreatePipelineLayout(device,&plci,nullptr,&layout));
    const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}}};VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpci.maxSets=3;dpci.poolSizeCount=2;dpci.pPoolSizes=sizes.data();VkDescriptorPool descriptorPool{};OK(vkCreateDescriptorPool(device,&dpci,nullptr,&descriptorPool));
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cpci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool commandPool{};OK(vkCreateCommandPool(device,&cpci,nullptr,&commandPool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cbai.commandPool=commandPool;cbai.commandBufferCount=1;VkCommandBuffer command{};OK(vkAllocateCommandBuffers(device,&cbai,&command));
    for(uint32_t plane=0;plane<3;++plane) {
        const bool depth=plane==1, stencilPlane=plane==2;const uint32_t bytes=stencilPlane?1:4;
        void* mapped{};OK(vkMapMemory(device,outputMemory,0,4096,0,&mapped));std::memset(mapped,0xA5,4096);vkUnmapMemory(device,outputMemory);
        auto spv=CompileReadback(depth,stencilPlane);VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sci.codeSize=spv.size()*4;sci.pCode=spv.data();VkShaderModule shader{};OK(vkCreateShaderModule(device,&sci,nullptr,&shader));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pci.layout=layout;pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};VkPipeline compute{};OK(vkCreateComputePipelines(device,pipelineCache,1,&pci,nullptr,&compute));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vci.image=images[plane?1:0];vci.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;vci.format=plane?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R32_UINT;vci.subresourceRange={plane?(depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT):VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view{};OK(vkCreateImageView(device,&vci,nullptr,&view));
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sai.descriptorPool=descriptorPool;sai.descriptorSetCount=1;sai.pSetLayouts=&setLayout;VkDescriptorSet set{};OK(vkAllocateDescriptorSets(device,&sai,&set));
        VkDescriptorImageInfo dii{nullptr,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkDescriptorBufferInfo dbi{buffer,0,4096};
        std::array<VkWriteDescriptorSet,2> writes{{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,0,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&dii,nullptr,nullptr},{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&dbi,nullptr}}};vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
        OK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(command,&begin));vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,compute);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        std::array<uint32_t,9> params{16,6,5,4,4,1,4,0,0};vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),params.data());vkCmdDispatch(command,1,1,1);OK(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;OK(vkQueueSubmit(queue,1,&submit,{}));OK(vkQueueWaitIdle(queue));
        OK(vkMapMemory(device,outputMemory,0,4096,0,&mapped));
        std::array<uint8_t,4096> expected;expected.fill(0xA5);
        for(uint32_t y=0;y<4;++y)for(uint32_t x=0;x<4;++x)for(uint32_t sample=0;sample<4;++sample) {
            uint32_t value=(17+sample)|((33+sample)<<8)|((65+sample)<<16)|0xFF000000;
            if(depth){const float f=float(sample+1)/8;std::memcpy(&value,&f,4);}if(stencilPlane)value=17+sample;
            std::memcpy(expected.data()+16+((y*6+x)*4+sample)*bytes,&value,bytes);
        }
        if(std::memcmp(mapped,expected.data(),4096)!=0){for(size_t i=0;i<4096;++i)if(static_cast<uint8_t*>(mapped)[i]!=expected[i]){std::fprintf(stderr,"plane %u byte %zu: got %02x expected %02x\n",plane,i,static_cast<uint8_t*>(mapped)[i],expected[i]);break;}std::abort();}
        vkUnmapMemory(device,outputMemory);vkDestroyImageView(device,view,nullptr);vkDestroyPipeline(device,compute,nullptr);vkDestroyShaderModule(device,shader,nullptr);
    }

    // Two GPU-counted draws split one logical pass twice. A preceding direct
    // draw proves attachment preservation; final STORE/DONT_CARE and optional
    // MSAA resolve must retain their original Vulkan meaning after both splits.
    for (bool resolve : {false,true}) {
    VkImageCreateInfo colorInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    colorInfo.imageType=VK_IMAGE_TYPE_2D;colorInfo.format=VK_FORMAT_R8G8B8A8_UNORM;
    colorInfo.extent={4,4,1};colorInfo.mipLevels=colorInfo.arrayLayers=1;colorInfo.samples=resolve?VK_SAMPLE_COUNT_4_BIT:VK_SAMPLE_COUNT_1_BIT;
    colorInfo.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage colorImage{};OK(vkCreateImage(device,&colorInfo,nullptr,&colorImage));
    VkMemoryRequirements colorRequirements{};vkGetImageMemoryRequirements(device,colorImage,&colorRequirements);
    VkMemoryAllocateInfo colorAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};colorAllocation.allocationSize=colorRequirements.size;
    VkDeviceMemory colorMemory{};OK(vkAllocateMemory(device,&colorAllocation,nullptr,&colorMemory));OK(vkBindImageMemory(device,colorImage,colorMemory,0));
    VkImageViewCreateInfo colorViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};colorViewInfo.image=colorImage;colorViewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;colorViewInfo.format=colorInfo.format;colorViewInfo.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView colorView{};OK(vkCreateImageView(device,&colorViewInfo,nullptr,&colorView));
    VkImage resolveImage{};VkDeviceMemory resolveMemory{};VkImageView resolveView{};
    if(resolve) {
        auto resolveInfo=colorInfo;resolveInfo.samples=VK_SAMPLE_COUNT_1_BIT;
        OK(vkCreateImage(device,&resolveInfo,nullptr,&resolveImage));
        vkGetImageMemoryRequirements(device,resolveImage,&colorRequirements);colorAllocation.allocationSize=colorRequirements.size;
        OK(vkAllocateMemory(device,&colorAllocation,nullptr,&resolveMemory));OK(vkBindImageMemory(device,resolveImage,resolveMemory,0));
        auto resolveViewInfo=colorViewInfo;resolveViewInfo.image=resolveImage;
        OK(vkCreateImageView(device,&resolveViewInfo,nullptr,&resolveView));
    }
    auto vertexWords=CompileSource("#version 450\nvoid main(){vec2 p[3]=vec2[](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexIndex],0,1);}",EShLangVertex);
    auto fragmentWords=CompileSource("#version 450\nlayout(location=0) out vec4 color;void main(){color=vec4(0.125,0,0,0);}",EShLangFragment);
    std::array<VkShaderModule,2> modules{};
    for(size_t i=0;i<2;++i){const auto& words=i?fragmentWords:vertexWords;VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();OK(vkCreateShaderModule(device,&ci,nullptr,&modules[i]));}
    VkPipelineLayoutCreateInfo emptyLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};VkPipelineLayout drawLayout{};OK(vkCreatePipelineLayout(device,&emptyLayoutInfo,nullptr,&drawLayout));
    std::array<VkPipelineShaderStageCreateInfo,2> stages{{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,modules[0],"main",nullptr},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,modules[1],"main",nullptr}}};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,4,4,0,1};VkRect2D scissor{{0,0},{4,4}};
    VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewports.viewportCount=viewports.scissorCount=1;viewports.pViewports=&viewport;viewports.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=colorInfo.samples;
    VkPipelineColorBlendAttachmentState blendAttachment{};blendAttachment.blendEnable=VK_TRUE;blendAttachment.srcColorBlendFactor=blendAttachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;blendAttachment.srcAlphaBlendFactor=blendAttachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blendAttachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&blendAttachment;
    VkPipelineRenderingCreateInfo pipelineRendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};pipelineRendering.colorAttachmentCount=1;pipelineRendering.pColorAttachmentFormats=&colorInfo.format;
    VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,&pipelineRendering};graphics.stageCount=2;graphics.pStages=stages.data();graphics.pVertexInputState=&vertexInput;graphics.pInputAssemblyState=&assembly;graphics.pViewportState=&viewports;graphics.pRasterizationState=&raster;graphics.pMultisampleState=&multisample;graphics.pColorBlendState=&blend;graphics.layout=drawLayout;
    VkPipeline drawPipeline{};OK(vkCreateGraphicsPipelines(device,pipelineCache,1,&graphics,nullptr,&drawPipeline));
    void* arguments{};OK(vkMapMemory(device,outputMemory,0,4096,0,&arguments));
    const std::array<VkDrawIndirectCommand,2> directArgs{{{3,1,0,0},{3,1,0,0}}};std::memcpy(static_cast<char*>(arguments)+1024,directArgs.data(),sizeof(directArgs));
    const std::array<VkDrawIndexedIndirectCommand,2> indexedArgs{{{3,1,0,0,0},{3,1,0,0,0}}};std::memcpy(static_cast<char*>(arguments)+1408,indexedArgs.data(),sizeof(indexedArgs));
    const uint32_t indices[3]={0,1,2};std::memcpy(static_cast<char*>(arguments)+2304,indices,sizeof(indices));vkUnmapMemory(device,outputMemory);
    for(bool store:{false,true})for(bool indexed:{false,true})for(uint32_t requested:{0u,1u,2u,7u}) {
        OK(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};OK(vkBeginCommandBuffer(command,&begin));
        vkCmdFillBuffer(command,buffer,2048,4,requested);VkDependencyInfo barrier{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};vkCmdPipelineBarrier2(command,&barrier);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=colorView;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;color.storeOp=store?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if(resolve){color.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;color.resolveImageView=resolveView;color.resolveImageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;}
        VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};render.renderArea.extent={4,4};render.layerCount=1;render.colorAttachmentCount=1;render.pColorAttachments=&color;
        vkCmdBeginRendering(command,&render);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,drawPipeline);
        vkCmdDraw(command,3,1,0,0);
        if(indexed) vkCmdBindIndexBuffer(command,buffer,2304,VK_INDEX_TYPE_UINT32);
        for(unsigned repeat=0;repeat<2;++repeat) {
            if(indexed) vkCmdDrawIndexedIndirectCount(command,buffer,1408,buffer,2048,2,sizeof(VkDrawIndexedIndirectCommand));
            else vkCmdDrawIndirectCount(command,buffer,1024,buffer,2048,2,sizeof(VkDrawIndirectCommand));
        }
        vkCmdEndRendering(command);vkCmdPipelineBarrier2(command,&barrier);
        VkBufferImageCopy readback{};readback.bufferOffset=3072;readback.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};readback.imageExtent={4,4,1};
        if(store||resolve) vkCmdCopyImageToBuffer(command,resolve?resolveImage:colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&readback);
        OK(vkEndCommandBuffer(command));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;OK(vkQueueSubmit(queue,1,&submit,{}));OK(vkQueueWaitIdle(queue));
        if(store||resolve) {
            OK(vkMapMemory(device,outputMemory,0,4096,0,&arguments));const uint32_t expected=(1+2*std::min(requested,2u))*32;
            for(size_t i=0;i<16;++i){const auto actual=static_cast<uint32_t*>(arguments)[3072/4+i];if(actual!=expected){std::fprintf(stderr,"indirect resolve=%u store=%u indexed=%u count=%u pixel=%zu got=%08x expected=%08x\n",resolve,store,indexed,requested,i,actual,expected);std::abort();}}
            vkUnmapMemory(device,outputMemory);
        }
    }
    vkDestroyPipeline(device,drawPipeline,nullptr);vkDestroyPipelineLayout(device,drawLayout,nullptr);for(auto module:modules)vkDestroyShaderModule(device,module,nullptr);vkDestroyImageView(device,colorView,nullptr);vkDestroyImage(device,colorImage,nullptr);vkFreeMemory(device,colorMemory,nullptr);
    if(resolve){vkDestroyImageView(device,resolveView,nullptr);vkDestroyImage(device,resolveImage,nullptr);vkFreeMemory(device,resolveMemory,nullptr);}
    }
    vkDestroyPipelineCache(device,pipelineCache,nullptr);
    vkDestroyCommandPool(device,commandPool,nullptr);vkDestroyDescriptorPool(device,descriptorPool,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,setLayout,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,outputMemory,nullptr);
    for(size_t i=0;i<2;++i){vkDestroyImage(device,images[i],nullptr);vkFreeMemory(device,imageMemory[i],nullptr);}vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);glslang::FinalizeProcess();
    std::puts("MSAA raw color, depth and stencil; repeated GPU counted draws, STORE/DONT_CARE and final MSAA resolve PASS");
}}
