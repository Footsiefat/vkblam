#include <Blam/Blam.hpp>
#include <Blam/Enums.hpp>
#include <Vulkan/VulkanAPI.hpp>
#include <vulkan/vulkan_enums.hpp>

#define GLM_FORCE_RADIANS
//#define GLM_FORCE_DEPTH_ZERO_TO_ONE
//#define GLM_FORCE_LEFT_HANDED
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>
#include <glm/gtx/vec_swizzle.hpp>

#pragma once

namespace VkBlam
{

vk::ImageType BlamToVk(Blam::BitmapEntryType Value);

vk::Format BlamToVk(Blam::BitmapEntryFormat Value);

//// Must match vkBlam.glsl structures

struct CameraGlobals
{
	alignas(16) glm::f32mat4x4 View;
	alignas(16) glm::f32mat4x4 Projection;
	alignas(16) glm::f32mat4x4 ViewProjection;
};

struct SimulationGlobals
{
	alignas(16) glm::float32_t Time;
};
struct PassGlobals
{
	alignas(16) glm::f32vec4 ScreenSize; // {width, height, 1/width, 1/height}
};

} // namespace VkBlam