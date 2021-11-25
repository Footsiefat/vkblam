#include "Blam/Types.hpp"
#include "Blam/Util.hpp"
#include "Common/Format.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <span>

#include <Common/Alignment.hpp>
#include <Common/Literals.hpp>

#include <Vulkan/Debug.hpp>
#include <Vulkan/Memory.hpp>
#include <Vulkan/VulkanAPI.hpp>

#include <mio/mmap.hpp>

#include <cmrc/cmrc.hpp>
CMRC_DECLARE(vkblam);
auto DataFS = cmrc::vkblam::get_filesystem();

#include <Blam/Blam.hpp>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_LEFT_HANDED
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include "stb_image_write.h"

//#define CAPTURE
#ifdef CAPTURE
#include <dlfcn.h>
#include <renderdoc_app.h>
RENDERDOC_API_1_4_1* rdoc_api = NULL;
#endif

static constexpr glm::uvec2              RenderSize = {1024, 1024};
static constexpr vk::SampleCountFlagBits RenderSamples
	= vk::SampleCountFlagBits::e4;

vk::UniqueDescriptorPool CreateMainDescriptorPool(vk::Device Device);

vk::UniqueRenderPass CreateMainRenderPass(
	vk::Device              Device,
	vk::SampleCountFlagBits SampleCount = vk::SampleCountFlagBits::e1);

vk::UniqueFramebuffer CreateMainFrameBuffer(
	vk::Device Device, vk::ImageView Color, vk::ImageView DepthAA,
	vk::ImageView ColorAA, glm::uvec2 ImageSize, vk::RenderPass RenderPass);

std::tuple<
	vk::UniquePipeline, vk::UniquePipelineLayout, vk::UniqueDescriptorSetLayout>
	CreateGraphicsPipeline(
		vk::Device                                         Device,
		const std::vector<vk::PushConstantRange>&          PushConstants,
		const std::vector<vk::DescriptorSetLayoutBinding>& Bindings,
		vk::ShaderModule VertModule, vk::ShaderModule FragModule,
		vk::RenderPass  RenderPass,
		vk::PolygonMode PolygonMode = vk::PolygonMode::eFill);

vk::UniqueShaderModule CreateShaderModule(
	vk::Device Device, std::span<const std::uint32_t> ShaderCode)
{
	vk::ShaderModuleCreateInfo ShaderInfo{};
	ShaderInfo.pCode    = ShaderCode.data();
	ShaderInfo.codeSize = ShaderCode.size_bytes();

	vk::UniqueShaderModule ShaderModule{};
	if( auto CreateResult = Device.createShaderModuleUnique(ShaderInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		ShaderModule = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Failed to create shader module: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}
	return ShaderModule;
}

int main(int argc, char* argv[])
{
	using namespace Common::Literals;

	if( argc < 3 )
	{
		// Not enough arguments
		return EXIT_FAILURE;
	}

#ifdef CAPTURE
	void* mod = dlopen("librenderdoc.so", RTLD_NOW);
	char* msg = dlerror();
	if( msg )
		std::puts(msg);
	if( mod )
	{
		pRENDERDOC_GetAPI RENDERDOC_GetAPI
			= (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
		int ret
			= RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_4_1, (void**)&rdoc_api);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_DebugOutputMute, 0);
		assert(ret == 1);
	}
#endif

	std::filesystem::path CurPath(argv[1]);
	std::filesystem::path BitmapPath(argv[2]);
	auto                  MapFile    = mio::mmap_source(CurPath.c_str());
	auto                  BitmapFile = mio::mmap_source(BitmapPath.c_str());

	Blam::MapFile CurMap(std::span<const std::byte>(
		reinterpret_cast<const std::byte*>(MapFile.data()), MapFile.size()));

	std::fputs(Blam::ToString(CurMap.MapHeader).c_str(), stdout);
	std::fputs(Blam::ToString(CurMap.TagIndexHeader).c_str(), stdout);

	glm::f32vec3 WorldBoundMax(std::numeric_limits<float>::min());
	glm::f32vec3 WorldBoundMin(std::numeric_limits<float>::max());

	//// Create Instance
	vk::InstanceCreateInfo InstanceInfo = {};

	vk::ApplicationInfo ApplicationInfo = {};
	ApplicationInfo.apiVersion          = VK_API_VERSION_1_1;

	ApplicationInfo.pEngineName   = "vkblam";
	ApplicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

	ApplicationInfo.pApplicationName   = "vkblam";
	ApplicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);

	InstanceInfo.pApplicationInfo = &ApplicationInfo;

	static const char* InstanceExtensions[]
		= {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

	InstanceInfo.ppEnabledExtensionNames = InstanceExtensions;
	InstanceInfo.enabledExtensionCount   = std::size(InstanceExtensions);

	vk::UniqueInstance Instance = {};

	if( auto CreateResult = vk::createInstanceUnique(InstanceInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		Instance = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating Vulkan instance: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(Instance.get());

	//// Pick physical device
	vk::PhysicalDevice PhysicalDevice = {};

	if( auto EnumerateResult = Instance->enumeratePhysicalDevices();
		EnumerateResult.result == vk::Result::eSuccess )
	{
		for( const auto& CurPhysicalDevice : EnumerateResult.value )
		{
			// Just pick the first physical device
			if( CurPhysicalDevice.getProperties().deviceType
				== vk::PhysicalDeviceType::eDiscreteGpu )
			{
				PhysicalDevice = CurPhysicalDevice;
				break;
			}
		}
	}
	else
	{
		std::fprintf(
			stderr, "Error enumerating physical devices: %s\n",
			vk::to_string(EnumerateResult.result).c_str());
		return EXIT_FAILURE;
	}

	// We're putting images/buffers right after each other, so we need to
	// ensure they are far apart enough to not be considered aliasing
	const std::size_t BufferImageGranularity
		= PhysicalDevice.getProperties().limits.bufferImageGranularity;

	//// Create Device
	vk::DeviceCreateInfo DeviceInfo = {};

	vk::PhysicalDeviceFeatures DeviceFeatures = {};
	DeviceFeatures.sampleRateShading          = true;
	DeviceFeatures.wideLines                  = true;
	DeviceFeatures.fillModeNonSolid           = true;
	DeviceInfo.pEnabledFeatures               = &DeviceFeatures;

	static const float QueuePriority = 1.0f;

	vk::DeviceQueueCreateInfo QueueInfo = {};
	QueueInfo.queueFamilyIndex          = 0;
	QueueInfo.queueCount                = 1;
	QueueInfo.pQueuePriorities          = &QueuePriority;

	DeviceInfo.queueCreateInfoCount = 1;
	DeviceInfo.pQueueCreateInfos    = &QueueInfo;

	vk::UniqueDevice Device = {};
	if( auto CreateResult = PhysicalDevice.createDeviceUnique(DeviceInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		Device = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating logical device: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	VULKAN_HPP_DEFAULT_DISPATCHER.init(Device.get());

#ifdef CAPTURE
	if( rdoc_api )
		rdoc_api->StartFrameCapture(
			*(void**)(VkInstance)(*Instance.operator->()), NULL);
#endif

	// Main Rendering queue
	vk::Queue RenderQueue = Device->getQueue(0, 0);

	//// Create Descriptor Pool
	vk::UniqueDescriptorPool MainDescriptorPool
		= CreateMainDescriptorPool(Device.get());

	//// Main Render Pass
	vk::UniqueRenderPass MainRenderPass
		= CreateMainRenderPass(Device.get(), RenderSamples);

	// Buffers
	vk::UniqueBuffer       StagingBuffer              = {};
	vk::UniqueDeviceMemory StagingBufferMemory        = {};
	std::size_t            StagingBufferWritePosition = 0;

	vk::BufferCreateInfo StagingBufferInfo = {};
	StagingBufferInfo.size                 = std::max(
						128_MiB, RenderSize.x * RenderSize.y * sizeof(std::uint32_t));
	StagingBufferInfo.usage = vk::BufferUsageFlagBits::eTransferDst
							| vk::BufferUsageFlagBits::eTransferSrc;

	if( auto CreateResult = Device->createBufferUnique(StagingBufferInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		StagingBuffer = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating staging buffer: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	Vulkan::SetObjectName(
		Device.get(), StagingBuffer.get(), "Staging Buffer( %s )",
		Common::FormatByteCount(StagingBufferInfo.size).c_str());

	// Allocate memory for staging buffer
	{
		const vk::MemoryRequirements StagingBufferMemoryRequirements
			= Device->getBufferMemoryRequirements(StagingBuffer.get());

		vk::MemoryAllocateInfo StagingBufferAllocInfo = {};
		StagingBufferAllocInfo.allocationSize
			= StagingBufferMemoryRequirements.size;
		StagingBufferAllocInfo.memoryTypeIndex = Vulkan::FindMemoryTypeIndex(
			PhysicalDevice, StagingBufferMemoryRequirements.memoryTypeBits,
			vk::MemoryPropertyFlagBits::eHostVisible
				| vk::MemoryPropertyFlagBits::eHostCoherent);

		if( auto AllocResult
			= Device->allocateMemoryUnique(StagingBufferAllocInfo);
			AllocResult.result == vk::Result::eSuccess )
		{
			StagingBufferMemory = std::move(AllocResult.value);
		}
		else
		{
			std::fprintf(
				stderr, "Error allocating memory for staging buffer: %s\n",
				vk::to_string(AllocResult.result).c_str());
			return EXIT_FAILURE;
		}

		if( auto BindResult = Device->bindBufferMemory(
				StagingBuffer.get(), StagingBufferMemory.get(), 0);
			BindResult == vk::Result::eSuccess )
		{
			// Successfully binded memory to buffer
		}
		else
		{
			std::fprintf(
				stderr, "Error binding memory to staging buffer: %s\n",
				vk::to_string(BindResult).c_str());
			return EXIT_FAILURE;
		}
	}

	std::span<std::byte> StagingBufferData;
	if( auto MapResult = Device->mapMemory(
			StagingBufferMemory.get(), 0, StagingBufferInfo.size);
		MapResult.result == vk::Result::eSuccess )
	{
		StagingBufferData = std::span<std::byte>(
			reinterpret_cast<std::byte*>(MapResult.value),
			StagingBufferInfo.size);
	}
	else
	{
		std::fprintf(
			stderr, "Error mapping staging buffer memory: %s\n",
			vk::to_string(MapResult.result).c_str());
		return EXIT_FAILURE;
	}

	std::unordered_map<vk::Image, vk::BufferImageCopy> ImageUploads;

	vk::UniqueBuffer            VertexBuffer;
	std::vector<vk::BufferCopy> VertexBufferCopies;

	vk::UniqueBuffer            LightmapVertexBuffer;
	std::vector<vk::BufferCopy> LightmapVertexBufferCopies;

	vk::UniqueBuffer            IndexBuffer;
	std::vector<vk::BufferCopy> IndexBufferCopies;

	// Contains _both_ the vertex buffer and the index buffer
	vk::UniqueDeviceMemory GeometryBufferHeapMemory = {};

	// TagID -> BitmapTag[indexofimage] -> vulkan image
	std::unordered_map<std::uint32_t, std::map<std::uint16_t, vk::UniqueImage>>
		Lightmaps;
	std::unordered_map<
		std::uint32_t, std::map<std::uint16_t, vk::UniqueImageView>>
		LightmapViews;

	std::unordered_map<
		std::uint32_t, std::map<std::uint16_t, vk::UniqueDescriptorSet>>
		LightmapSets;

	// Parameters used for drawing
	std::vector<std::uint32_t> VertexIndexOffsets;
	std::vector<std::uint32_t> IndexCounts;
	std::vector<std::uint32_t> IndexOffsets;

	// Draw index -> {bitmapID, bitmap index}
	struct LightmapIndexMapping
	{
		std::uint32_t BitmapID;
		std::int32_t  BitmapIndex;
	};
	std::vector<LightmapIndexMapping> LightmapIndex;

	{
		// Offset is in elements, not bytes
		std::uint32_t VertexHeapIndexOffset = 0;
		std::uint32_t IndexHeapIndexOffset  = 0;

		if( const auto BaseTagPtr
			= CurMap.GetTagIndexEntry(CurMap.TagIndexHeader.BaseTag);
			BaseTagPtr )
		{
			const auto&            CurTag = *BaseTagPtr;
			const std::string_view TagName
				= CurMap.GetTagName(CurMap.TagIndexHeader.BaseTag);

			if( const auto ScenarioPtr
				= CurMap.GetTag<Blam::TagClass::Scenario>(
					CurMap.TagIndexHeader.BaseTag);
				ScenarioPtr )
			{
				const Blam::Tag<Blam::TagClass::Scenario>& Scenario
					= *ScenarioPtr;

				for( const Blam::Tag<Blam::TagClass::Scenario>::StructureBSP&
						 CurBSPEntry : Scenario.StructureBSPs.GetSpan(
							 MapFile.data(), CurMap.TagHeapVirtualBase) )
				{
					const std::span<const std::byte> BSPData
						= CurBSPEntry.GetSBSPData(MapFile.data());
					const Blam::Tag<Blam::TagClass::ScenarioStructureBsp>&
						ScenarioBSP
						= CurBSPEntry.GetSBSP(MapFile.data());

					WorldBoundMin.x = glm::min(
						WorldBoundMin.x, ScenarioBSP.WorldBoundsX[0]);
					WorldBoundMin.y = glm::min(
						WorldBoundMin.y, ScenarioBSP.WorldBoundsY[0]);
					WorldBoundMin.z = glm::min(
						WorldBoundMin.z, ScenarioBSP.WorldBoundsZ[0]);

					WorldBoundMax.x = glm::max(
						WorldBoundMax.x, ScenarioBSP.WorldBoundsX[1]);
					WorldBoundMax.y = glm::max(
						WorldBoundMax.y, ScenarioBSP.WorldBoundsY[1]);
					WorldBoundMax.z = glm::max(
						WorldBoundMax.z, ScenarioBSP.WorldBoundsZ[1]);
					// Lightmap
					for( const auto& CurLightmap :
						 ScenarioBSP.Lightmaps.GetSpan(
							 BSPData.data(), CurBSPEntry.BSPVirtualBase) )
					{
						const auto& LightmapTextureTag
							= CurMap.GetTag<Blam::TagClass::Bitmap>(
								ScenarioBSP.LightmapTexture.TagID);
						const std::int16_t LightmapTextureIndex
							= CurLightmap.LightmapIndex;

						auto& LightmapImage
							= Lightmaps[ScenarioBSP.LightmapTexture.TagID]
									   [LightmapTextureIndex];

						if( !LightmapImage && (LightmapTextureIndex >= 0) )
						{
							const auto& LightmapSubTexture
								= LightmapTextureTag->Bitmaps.GetSpan(
									MapFile.data(), CurMap.TagHeapVirtualBase)
									  [LightmapTextureIndex];
							const auto PixelData = std::span<const std::byte>(
								reinterpret_cast<const std::byte*>(
									BitmapFile.data())
									+ LightmapSubTexture.PixelDataOffset,
								LightmapSubTexture.PixelDataSize);

							vk::ImageCreateInfo LightmapImageInfo = {};
							LightmapImageInfo.imageType = vk::ImageType::e2D;
							LightmapImageInfo.format
								= vk::Format::eR5G6B5UnormPack16;
							LightmapImageInfo.extent = vk::Extent3D(
								LightmapSubTexture.Width,
								LightmapSubTexture.Height,
								LightmapSubTexture.Depth);
							LightmapImageInfo.mipLevels   = 1;
							LightmapImageInfo.arrayLayers = 1;
							LightmapImageInfo.samples
								= vk::SampleCountFlagBits::e1;
							LightmapImageInfo.tiling
								= vk::ImageTiling::eOptimal;
							LightmapImageInfo.usage
								= vk::ImageUsageFlagBits::eSampled
								| vk::ImageUsageFlagBits::eTransferDst
								| vk::ImageUsageFlagBits::eTransferSrc;
							LightmapImageInfo.sharingMode
								= vk::SharingMode::eExclusive;
							LightmapImageInfo.initialLayout
								= vk::ImageLayout::eUndefined;

							if( auto CreateResult
								= Device->createImageUnique(LightmapImageInfo);
								CreateResult.result == vk::Result::eSuccess )
							{
								LightmapImage = std::move(CreateResult.value);
							}
							else
							{
								std::fprintf(
									stderr,
									"Error creating render target: %s\n",
									vk::to_string(CreateResult.result).c_str());
								return EXIT_FAILURE;
							}
							Vulkan::SetObjectName(
								Device.get(), LightmapImage.get(),
								"Lightmap Image %08X[%2zu]",
								ScenarioBSP.LightmapTexture.TagID,
								LightmapTextureIndex);

							std::memcpy(
								StagingBufferData.data()
									+ StagingBufferWritePosition,
								PixelData.data(), PixelData.size_bytes());

							ImageUploads[LightmapImage.get()]
								= vk::BufferImageCopy(
									StagingBufferWritePosition, 0, 0,
									vk::ImageSubresourceLayers(
										vk::ImageAspectFlagBits::eColor, 0, 0,
										1),
									vk::Offset3D(0, 0, 0),
									LightmapImageInfo.extent);

							StagingBufferWritePosition
								+= PixelData.size_bytes();
						}

						const auto Surfaces = ScenarioBSP.Surfaces.GetSpan(
							BSPData.data(), CurBSPEntry.BSPVirtualBase);
						for( const auto& CurMaterial :
							 CurLightmap.Materials.GetSpan(
								 BSPData.data(), CurBSPEntry.BSPVirtualBase) )
						{

							//// Vertex Buffer data
							{
								// Copy vertex data into the staging buffer
								const std::span<const Blam::Vertex>
									CurVertexData = CurMaterial.GetVertices(
										BSPData.data(),
										CurBSPEntry.BSPVirtualBase);
								std::memcpy(
									StagingBufferData.data()
										+ StagingBufferWritePosition,
									CurVertexData.data(),
									CurVertexData.size_bytes());

								// Queue up staging buffer copy
								VertexBufferCopies.emplace_back(vk::BufferCopy(
									StagingBufferWritePosition,
									VertexHeapIndexOffset
										* sizeof(Blam::Vertex),
									CurVertexData.size_bytes()));

								// Add the offset needed to begin indexing into
								// this particular part of the vertex buffer,
								// used when drawing
								VertexIndexOffsets.emplace_back(
									VertexHeapIndexOffset);

								LightmapIndex.emplace_back(
									ScenarioBSP.LightmapTexture.TagID,
									LightmapTextureIndex);

								StagingBufferWritePosition
									+= CurVertexData.size_bytes();

								//// Lightmap vertex buffer data
								{
									const std::span<const Blam::LightmapVertex>
										CurLightmapVertexData
										= CurMaterial.GetLightmapVertices(
											BSPData.data(),
											CurBSPEntry.BSPVirtualBase);

									const auto test
										= CurLightmapVertexData.data();

									// Copy lightmap vertex data into the
									// staging buffer
									std::memcpy(
										StagingBufferData.data()
											+ StagingBufferWritePosition,
										CurLightmapVertexData.data(),
										CurLightmapVertexData.size_bytes());

									// Queue up staging buffer copy
									LightmapVertexBufferCopies.emplace_back(
										vk::BufferCopy(
											StagingBufferWritePosition,
											VertexHeapIndexOffset
												* sizeof(Blam::LightmapVertex),
											CurLightmapVertexData
												.size_bytes()));

									StagingBufferWritePosition
										+= CurLightmapVertexData.size_bytes();
								}

								VertexHeapIndexOffset += CurVertexData.size();
							}

							//// Index Buffer data
							{
								const std::span<const std::byte> CurIndexData
									= std::as_bytes(Surfaces.subspan(
										CurMaterial.SurfacesIndexStart,
										CurMaterial.SurfacesCount));

								IndexCounts.emplace_back(
									CurMaterial.SurfacesCount * 3);

								IndexOffsets.emplace_back(IndexHeapIndexOffset);
								// Copy into the staging buffer
								std::memcpy(
									StagingBufferData.data()
										+ StagingBufferWritePosition,
									CurIndexData.data(),
									CurIndexData.size_bytes());

								// Queue up staging buffer copy
								IndexBufferCopies.emplace_back(vk::BufferCopy(
									StagingBufferWritePosition,
									IndexHeapIndexOffset
										* sizeof(std::uint16_t),
									CurIndexData.size_bytes()));

								// Increment offsets
								IndexHeapIndexOffset
									+= CurMaterial.SurfacesCount * 3;
								StagingBufferWritePosition
									+= CurIndexData.size_bytes();
							}
						}
					}
				}
			}
		}

		//// Create Vertex buffer heap
		vk::BufferCreateInfo VertexBufferInfo = {};
		VertexBufferInfo.size  = VertexHeapIndexOffset * sizeof(Blam::Vertex);
		VertexBufferInfo.usage = vk::BufferUsageFlagBits::eVertexBuffer
							   | vk::BufferUsageFlagBits::eTransferDst;

		if( auto CreateResult = Device->createBufferUnique(VertexBufferInfo);
			CreateResult.result == vk::Result::eSuccess )
		{
			VertexBuffer = std::move(CreateResult.value);
		}
		else
		{
			std::fprintf(
				stderr, "Error creating vertex buffer: %s\n",
				vk::to_string(CreateResult.result).c_str());
			return EXIT_FAILURE;
		}

		Vulkan::SetObjectName(
			Device.get(), VertexBuffer.get(), "Vertex Buffer( %s )",
			Common::FormatByteCount(VertexBufferInfo.size).c_str());

		//// Create Vertex buffer heap
		vk::BufferCreateInfo LightmapVertexBufferInfo = {};
		LightmapVertexBufferInfo.size
			= VertexHeapIndexOffset * sizeof(Blam::LightmapVertex);
		LightmapVertexBufferInfo.usage = vk::BufferUsageFlagBits::eVertexBuffer
									   | vk::BufferUsageFlagBits::eTransferDst;

		if( auto CreateResult
			= Device->createBufferUnique(LightmapVertexBufferInfo);
			CreateResult.result == vk::Result::eSuccess )
		{
			LightmapVertexBuffer = std::move(CreateResult.value);
		}
		else
		{
			std::fprintf(
				stderr, "Error creating lightmap vertex buffer: %s\n",
				vk::to_string(CreateResult.result).c_str());
			return EXIT_FAILURE;
		}

		Vulkan::SetObjectName(
			Device.get(), LightmapVertexBuffer.get(),
			"Lightmap Vertex Buffer( %s )",
			Common::FormatByteCount(LightmapVertexBufferInfo.size).c_str());

		//// Create Index buffer heap
		vk::BufferCreateInfo IndexBufferInfo = {};
		IndexBufferInfo.size  = IndexHeapIndexOffset * sizeof(std::uint16_t);
		IndexBufferInfo.usage = vk::BufferUsageFlagBits::eIndexBuffer
							  | vk::BufferUsageFlagBits::eTransferDst;

		if( auto CreateResult = Device->createBufferUnique(IndexBufferInfo);
			CreateResult.result == vk::Result::eSuccess )
		{
			IndexBuffer = std::move(CreateResult.value);
		}
		else
		{
			std::fprintf(
				stderr, "Error creating Index buffer: %s\n",
				vk::to_string(CreateResult.result).c_str());
			return EXIT_FAILURE;
		}
		Vulkan::SetObjectName(
			Device.get(), IndexBuffer.get(), "Index Buffer( %s )",
			Common::FormatByteCount(IndexBufferInfo.size).c_str());

		const vk::MemoryRequirements IndexBufferMemoryRequirements
			= Device->getBufferMemoryRequirements(IndexBuffer.get());

		if( auto [Result, Value] = Vulkan::CommitBufferHeap(
				Device.get(), PhysicalDevice,
				std::array{
					VertexBuffer.get(), IndexBuffer.get(),
					LightmapVertexBuffer.get()});
			Result == vk::Result::eSuccess )
		{
			GeometryBufferHeapMemory = std::move(Value);
		}
		else
		{
			std::fprintf(
				stderr, "Error committing vertex/index memory: %s\n",
				vk::to_string(Result).c_str());
			return EXIT_FAILURE;
		}

		Vulkan::SetObjectName(
			Device.get(), GeometryBufferHeapMemory.get(),
			"Geometry Buffer Memory");
	}

	// Render Target images
	vk::UniqueImage RenderImage;

	vk::UniqueImage RenderImageAA;

	vk::UniqueImage RenderImageDepth;

	// Render-image, R8G8B8A8_SRGB
	vk::ImageCreateInfo RenderImageInfo = {};
	RenderImageInfo.imageType           = vk::ImageType::e2D;
	RenderImageInfo.format              = vk::Format::eR8G8B8A8Srgb;
	RenderImageInfo.extent      = vk::Extent3D(RenderSize.x, RenderSize.y, 1);
	RenderImageInfo.mipLevels   = 1;
	RenderImageInfo.arrayLayers = 1;
	RenderImageInfo.samples     = vk::SampleCountFlagBits::e1;
	RenderImageInfo.tiling      = vk::ImageTiling::eOptimal;
	RenderImageInfo.usage       = vk::ImageUsageFlagBits::eColorAttachment
						  | vk::ImageUsageFlagBits::eTransferSrc;
	RenderImageInfo.sharingMode   = vk::SharingMode::eExclusive;
	RenderImageInfo.initialLayout = vk::ImageLayout::eUndefined;

	// Render-image(MSAA), R8G8B8A8_SRGB
	vk::ImageCreateInfo RenderImageAAInfo = {};
	RenderImageAAInfo.imageType           = vk::ImageType::e2D;
	RenderImageAAInfo.format              = vk::Format::eR8G8B8A8Srgb;
	RenderImageAAInfo.samples             = RenderSamples;
	RenderImageAAInfo.extent      = vk::Extent3D(RenderSize.x, RenderSize.y, 1);
	RenderImageAAInfo.mipLevels   = 1;
	RenderImageAAInfo.arrayLayers = 1;
	RenderImageAAInfo.tiling      = vk::ImageTiling::eOptimal;
	RenderImageAAInfo.usage       = vk::ImageUsageFlagBits::eColorAttachment;
	RenderImageAAInfo.sharingMode = vk::SharingMode::eExclusive;
	RenderImageAAInfo.initialLayout = vk::ImageLayout::eUndefined;

	// Render-image-depth(MSAA), D32_sfloat
	vk::ImageCreateInfo RenderImageDepthInfo = {};
	RenderImageDepthInfo.imageType           = vk::ImageType::e2D;
	RenderImageDepthInfo.format              = vk::Format::eD32Sfloat;
	RenderImageDepthInfo.samples             = RenderSamples;
	RenderImageDepthInfo.extent = vk::Extent3D(RenderSize.x, RenderSize.y, 1);
	RenderImageDepthInfo.mipLevels   = 1;
	RenderImageDepthInfo.arrayLayers = 1;
	RenderImageDepthInfo.tiling      = vk::ImageTiling::eOptimal;
	RenderImageDepthInfo.usage
		= vk::ImageUsageFlagBits::eDepthStencilAttachment;
	RenderImageInfo.sharingMode   = vk::SharingMode::eExclusive;
	RenderImageInfo.initialLayout = vk::ImageLayout::eUndefined;

	if( auto CreateResult = Device->createImageUnique(RenderImageInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImage = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}
	Vulkan::SetObjectName(
		Device.get(), RenderImage.get(), "Render Image Resolve");

	if( auto CreateResult = Device->createImageUnique(RenderImageAAInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImageAA = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}
	Vulkan::SetObjectName(
		Device.get(), RenderImageAA.get(), "Render Image(AA)");

	if( auto CreateResult = Device->createImageUnique(RenderImageDepthInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImageDepth = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}
	Vulkan::SetObjectName(
		Device.get(), RenderImageDepth.get(), "Render Image Depth(AA)");

	std::vector<vk::Image> ImageHeapTargets = {};
	ImageHeapTargets.push_back(RenderImage.get());
	ImageHeapTargets.push_back(RenderImageAA.get());
	ImageHeapTargets.push_back(RenderImageDepth.get());

	for( const auto& [_, CurLightmapBitmap] : Lightmaps )
	{
		for( const auto& [_, CurLightmapBitmapSubimage] : CurLightmapBitmap )
		{
			if( CurLightmapBitmapSubimage )
			{
				ImageHeapTargets.push_back(CurLightmapBitmapSubimage.get());
			}
		}
	}

	// Allocate all the memory we need for these images up-front into a single
	// heap.
	vk::UniqueDeviceMemory ImageHeapMemory = {};

	if( auto [Result, Value] = Vulkan::CommitImageHeap(
			Device.get(), PhysicalDevice, ImageHeapTargets);
		Result == vk::Result::eSuccess )
	{
		ImageHeapMemory = std::move(Value);
	}
	else
	{
		std::fprintf(
			stderr, "Error committing image memory: %s\n",
			vk::to_string(Result).c_str());
		return EXIT_FAILURE;
	}

	//// Image Views
	// Create the image views for the render-targets
	vk::ImageViewCreateInfo ImageViewInfoTemplate{};
	ImageViewInfoTemplate.viewType     = vk::ImageViewType::e2D;
	ImageViewInfoTemplate.components.r = vk::ComponentSwizzle::eR;
	ImageViewInfoTemplate.components.g = vk::ComponentSwizzle::eG;
	ImageViewInfoTemplate.components.b = vk::ComponentSwizzle::eB;
	ImageViewInfoTemplate.components.a = vk::ComponentSwizzle::eA;
	ImageViewInfoTemplate.subresourceRange.aspectMask
		= vk::ImageAspectFlagBits::eColor;
	ImageViewInfoTemplate.subresourceRange.baseMipLevel   = 0;
	ImageViewInfoTemplate.subresourceRange.levelCount     = 1;
	ImageViewInfoTemplate.subresourceRange.baseArrayLayer = 0;
	ImageViewInfoTemplate.subresourceRange.layerCount     = 1;

	ImageViewInfoTemplate.image  = RenderImage.get();
	ImageViewInfoTemplate.format = RenderImageInfo.format;

	vk::UniqueImageView RenderImageView = {};
	if( auto CreateResult
		= Device->createImageViewUnique(ImageViewInfoTemplate);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImageView = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target view: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	ImageViewInfoTemplate.image           = RenderImageAA.get();
	ImageViewInfoTemplate.format          = RenderImageAAInfo.format;
	vk::UniqueImageView RenderImageAAView = {};
	if( auto CreateResult
		= Device->createImageViewUnique(ImageViewInfoTemplate);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImageAAView = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target view: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	ImageViewInfoTemplate.image  = RenderImageDepth.get();
	ImageViewInfoTemplate.format = RenderImageDepthInfo.format;
	ImageViewInfoTemplate.subresourceRange.aspectMask
		= vk::ImageAspectFlagBits::eDepth;
	vk::UniqueImageView RenderImageDepthView = {};
	if( auto CreateResult
		= Device->createImageViewUnique(ImageViewInfoTemplate);
		CreateResult.result == vk::Result::eSuccess )
	{
		RenderImageDepthView = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render target view: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	//// MainFrameBuffer
	vk::UniqueFramebuffer RenderFramebuffer = CreateMainFrameBuffer(
		Device.get(), RenderImageView.get(), RenderImageDepthView.get(),
		RenderImageAAView.get(), RenderSize, MainRenderPass.get());

	//// Create Default Sampler

	vk::SamplerCreateInfo SamplerInfo{};
	SamplerInfo.magFilter               = vk::Filter::eLinear;
	SamplerInfo.minFilter               = vk::Filter::eLinear;
	SamplerInfo.mipmapMode              = vk::SamplerMipmapMode::eLinear;
	SamplerInfo.addressModeU            = vk::SamplerAddressMode::eRepeat;
	SamplerInfo.addressModeV            = vk::SamplerAddressMode::eRepeat;
	SamplerInfo.addressModeW            = vk::SamplerAddressMode::eRepeat;
	SamplerInfo.mipLodBias              = 0.0f;
	SamplerInfo.anisotropyEnable        = VK_FALSE;
	SamplerInfo.maxAnisotropy           = 1.0f;
	SamplerInfo.compareEnable           = VK_FALSE;
	SamplerInfo.compareOp               = vk::CompareOp::eAlways;
	SamplerInfo.minLod                  = 0.0f;
	SamplerInfo.maxLod                  = 1.0f;
	SamplerInfo.borderColor             = vk::BorderColor::eFloatOpaqueWhite;
	SamplerInfo.unnormalizedCoordinates = VK_FALSE;

	vk::UniqueSampler DefaultSampler = {};
	if( auto CreateResult = Device->createSamplerUnique(SamplerInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		DefaultSampler = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating default sampler: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	// Main Shader modules
	const cmrc::file DefaultVertShaderData
		= DataFS.open("shaders/Default.vert.spv");
	const cmrc::file DefaultFragShaderData
		= DataFS.open("shaders/Default.frag.spv");
	const cmrc::file UnlitFragShaderData
		= DataFS.open("shaders/Unlit.frag.spv");

	vk::UniqueShaderModule DefaultVertexShaderModule = CreateShaderModule(
		Device.get(),
		std::span<const std::uint32_t>(
			reinterpret_cast<const std::uint32_t*>(
				DefaultVertShaderData.begin()),
			DefaultVertShaderData.size() / sizeof(std::uint32_t)));
	vk::UniqueShaderModule DefaultFragmentShaderModule = CreateShaderModule(
		Device.get(),
		std::span<const std::uint32_t>(
			reinterpret_cast<const std::uint32_t*>(
				DefaultFragShaderData.begin()),
			DefaultFragShaderData.size() / sizeof(std::uint32_t)));
	vk::UniqueShaderModule UnlitFragmentShaderModule = CreateShaderModule(
		Device.get(),
		std::span<const std::uint32_t>(
			reinterpret_cast<const std::uint32_t*>(UnlitFragShaderData.begin()),
			UnlitFragShaderData.size() / sizeof(std::uint32_t)));

	auto [DebugDrawPipeline, DebugDrawPipelineLayout, DebugDrawDescriptorLayout]
		= CreateGraphicsPipeline(
			Device.get(),
			{vk::PushConstantRange(
				vk::ShaderStageFlagBits::eAllGraphics, 0,
				sizeof(glm::f32mat4))},
			{
				vk::DescriptorSetLayoutBinding(
					0, vk::DescriptorType::eCombinedImageSampler, 1,
					vk::ShaderStageFlagBits::eFragment),
			},
			DefaultVertexShaderModule.get(), DefaultFragmentShaderModule.get(),
			MainRenderPass.get());

	//// Create lightmap image descriptor sets
	for( const auto& [LightmapTagID, CurLightmapBitmap] : Lightmaps )
	{
		for( const auto& [LightmapIndex, CurLightmapBitmapSubimage] :
			 CurLightmapBitmap )
		{
			if( CurLightmapBitmapSubimage )
			{
				auto& LightmapImageView
					= LightmapViews[LightmapTagID][LightmapIndex];
				vk::ImageViewCreateInfo LightmapImageViewInfo = {};
				LightmapImageViewInfo.image = CurLightmapBitmapSubimage.get();
				LightmapImageViewInfo.viewType = vk::ImageViewType::e2D;
				LightmapImageViewInfo.format   = vk::Format::eR5G6B5UnormPack16;
				LightmapImageViewInfo.subresourceRange.aspectMask
					= vk::ImageAspectFlagBits::eColor;
				LightmapImageViewInfo.subresourceRange.baseMipLevel   = 0;
				LightmapImageViewInfo.subresourceRange.levelCount     = 1;
				LightmapImageViewInfo.subresourceRange.baseArrayLayer = 0;
				LightmapImageViewInfo.subresourceRange.layerCount     = 1;

				if( auto CreateResult
					= Device->createImageViewUnique(LightmapImageViewInfo);
					CreateResult.result == vk::Result::eSuccess )
				{
					LightmapImageView = std::move(CreateResult.value);
				}
				else
				{
					std::fprintf(
						stderr, "Error creating render target: %s\n",
						vk::to_string(CreateResult.result).c_str());
					return EXIT_FAILURE;
				}

				vk::UniqueDescriptorSet& TargetSet
					= LightmapSets[LightmapTagID][LightmapIndex];
				vk::DescriptorSetAllocateInfo AllocInfo{};
				AllocInfo.descriptorPool     = MainDescriptorPool.get();
				AllocInfo.pSetLayouts        = &DebugDrawDescriptorLayout.get();
				AllocInfo.descriptorSetCount = 1;

				if( auto AllocResult
					= Device->allocateDescriptorSetsUnique(AllocInfo);
					AllocResult.result == vk::Result::eSuccess )
				{
					TargetSet = std::move(AllocResult.value[0]);
				}
				else
				{
					std::fprintf(
						stderr, "Error allocating descriptor set: %s\n",
						vk::to_string(AllocResult.result).c_str());
					return EXIT_FAILURE;
				}

				Vulkan::SetObjectName(
					Device.get(), TargetSet.get(), "Lightmap Image %08X[%2zu]",
					LightmapTagID, LightmapIndex);

				vk::DescriptorImageInfo ImageInfo{};
				ImageInfo.sampler     = DefaultSampler.get();
				ImageInfo.imageView   = LightmapImageView.get();
				ImageInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

				vk::WriteDescriptorSet WriteDescriptorSet{};
				WriteDescriptorSet.dstSet          = TargetSet.get();
				WriteDescriptorSet.dstBinding      = 0;
				WriteDescriptorSet.dstArrayElement = 0;
				WriteDescriptorSet.descriptorCount = 1;
				WriteDescriptorSet.descriptorType
					= vk::DescriptorType::eCombinedImageSampler;
				WriteDescriptorSet.pImageInfo = &ImageInfo;

				Device->updateDescriptorSets({WriteDescriptorSet}, {});
			}
		}
	}

	auto [UnlitDrawPipeline, UnlitDrawPipelineLayout, UnlitDrawDescriptorLayout]
		= CreateGraphicsPipeline(
			Device.get(),
			{vk::PushConstantRange(
				vk::ShaderStageFlagBits::eAllGraphics, 0,
				sizeof(glm::f32mat4) + sizeof(glm::f32vec4))},
			{vk::DescriptorSetLayoutBinding(
				0, vk::DescriptorType::eCombinedImageSampler, 1,
				vk::ShaderStageFlagBits::eFragment)},
			DefaultVertexShaderModule.get(), UnlitFragmentShaderModule.get(),
			MainRenderPass.get(), vk::PolygonMode::eLine);

	//// Create Command Pool
	vk::CommandPoolCreateInfo CommandPoolInfo = {};
	CommandPoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	CommandPoolInfo.queueFamilyIndex = 0;

	vk::UniqueCommandPool CommandPool = {};
	if( auto CreateResult = Device->createCommandPoolUnique(CommandPoolInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		CommandPool = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating command pool: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	//// Create Command Buffer
	vk::CommandBufferAllocateInfo CommandBufferInfo = {};
	CommandBufferInfo.commandPool                   = CommandPool.get();
	CommandBufferInfo.level              = vk::CommandBufferLevel::ePrimary;
	CommandBufferInfo.commandBufferCount = 1;

	vk::UniqueCommandBuffer CommandBuffer = {};

	if( auto AllocateResult
		= Device->allocateCommandBuffersUnique(CommandBufferInfo);
		AllocateResult.result == vk::Result::eSuccess )
	{
		CommandBuffer = std::move(AllocateResult.value[0]);
	}
	else
	{
		std::fprintf(
			stderr, "Error allocating command buffer: %s\n",
			vk::to_string(AllocateResult.result).c_str());
		return EXIT_FAILURE;
	}

	if( auto BeginResult = CommandBuffer->begin(vk::CommandBufferBeginInfo{});
		BeginResult != vk::Result::eSuccess )
	{
		std::fprintf(
			stderr, "Error beginning command buffer: %s\n",
			vk::to_string(BeginResult).c_str());
		return EXIT_FAILURE;
	}

	{
		Vulkan::DebugLabelScope DebugCopyScope(
			CommandBuffer.get(), {1.0, 0.0, 1.0, 1.0}, "Frame");

		// Flush all vertex buffer uploads
		{
			Vulkan::DebugLabelScope DebugCopyScope(
				CommandBuffer.get(), {1.0, 1.0, 0.0, 1.0},
				"Copy Vertex/Index Buffers");
			CommandBuffer->copyBuffer(
				StagingBuffer.get(), VertexBuffer.get(), VertexBufferCopies);
			CommandBuffer->copyBuffer(
				StagingBuffer.get(), LightmapVertexBuffer.get(),
				LightmapVertexBufferCopies);

			// Flush all vertex buffer uploads
			CommandBuffer->copyBuffer(
				StagingBuffer.get(), IndexBuffer.get(), IndexBufferCopies);
		}
		{
			Vulkan::DebugLabelScope DebugCopyScope(
				CommandBuffer.get(), {1.0, 1.0, 0.0, 1.0}, "Upload textures");

			for( const auto& [TargetImage, ImageCopy] : ImageUploads )
			{
				CommandBuffer->pipelineBarrier(
					vk::PipelineStageFlagBits::eTransfer,
					vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{},
					{}, {},
					{vk::ImageMemoryBarrier(
						vk::AccessFlagBits::eTransferWrite,
						vk::AccessFlagBits::eTransferRead,
						vk::ImageLayout::eUndefined,
						vk::ImageLayout::eTransferDstOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
						TargetImage,
						vk::ImageSubresourceRange(
							vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))});
			}
			for( const auto& [TargetImage, ImageCopy] : ImageUploads )
			{
				CommandBuffer->copyBufferToImage(
					StagingBuffer.get(), TargetImage,
					vk::ImageLayout::eTransferDstOptimal, ImageCopy);
			}
			for( const auto& [TargetImage, ImageCopy] : ImageUploads )
			{
				CommandBuffer->pipelineBarrier(
					vk::PipelineStageFlagBits::eTransfer,
					vk::PipelineStageFlagBits::eFragmentShader,
					vk::DependencyFlags{}, {}, {},
					{vk::ImageMemoryBarrier(
						vk::AccessFlagBits::eTransferWrite,
						vk::AccessFlagBits::eShaderRead,
						vk::ImageLayout::eTransferDstOptimal,
						vk::ImageLayout::eShaderReadOnlyOptimal,
						VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
						TargetImage,
						vk::ImageSubresourceRange(
							vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))});
			}
		}

		{
			Vulkan::DebugLabelScope DebugCopyScope(
				CommandBuffer.get(), {1.0, 1.0, 0.0, 1.0}, "Main Render Pass");

			vk::RenderPassBeginInfo RenderBeginInfo   = {};
			RenderBeginInfo.renderPass                = MainRenderPass.get();
			static const vk::ClearValue ClearColors[] = {
				vk::ClearColorValue(
					std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
				vk::ClearDepthStencilValue(1.0f, 0),
				vk::ClearColorValue(
					std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}),
			};
			RenderBeginInfo.pClearValues             = ClearColors;
			RenderBeginInfo.clearValueCount          = std::size(ClearColors);
			RenderBeginInfo.renderArea.extent.width  = RenderSize.x;
			RenderBeginInfo.renderArea.extent.height = RenderSize.y;
			RenderBeginInfo.framebuffer              = RenderFramebuffer.get();
			CommandBuffer->beginRenderPass(
				RenderBeginInfo, vk::SubpassContents::eInline);

			vk::Viewport Viewport = {};
			Viewport.width        = RenderSize.x;
			Viewport.height       = -float(RenderSize.y);
			Viewport.x            = 0;
			Viewport.y            = RenderSize.y;
			Viewport.minDepth     = 0.0f;
			Viewport.maxDepth     = 1.0f;
			CommandBuffer->setViewport(0, {Viewport});
			// Scissor
			vk::Rect2D Scissor    = {};
			Scissor.extent.width  = RenderSize.x;
			Scissor.extent.height = RenderSize.y;
			CommandBuffer->setScissor(0, {Scissor});
			// Draw
			CommandBuffer->bindPipeline(
				vk::PipelineBindPoint::eGraphics, DebugDrawPipeline.get());

			const glm::vec3 WorldCenter
				= glm::mix(WorldBoundMin, WorldBoundMax, 0.5);
			const glm::mat4 ViewMatrix = glm::lookAt<glm::f32>(
				glm::vec3(WorldCenter.x, WorldCenter.y, WorldBoundMax.z),
				glm::vec3(WorldCenter.x, WorldCenter.y, WorldBoundMin.z),
				glm::vec3(0, 1, 0));

			const glm::f32 MaxExtent
				= glm::compMax(WorldBoundMax - WorldBoundMin) / 2.0f;
			const glm::mat4 ProjectionMatrix = glm::ortho<glm::f32>(
				-MaxExtent, MaxExtent, -MaxExtent, MaxExtent, 0.0f,
				WorldBoundMax.z - WorldBoundMin.z);
			// = glm::perspective<glm::f32>(
			// 	glm::radians(60.0f),
			// 	static_cast<float>(RenderSize.x) / RenderSize.y, 0.1f, 1000.0f);

			const glm::mat4 ViewProjMatrix = ProjectionMatrix * ViewMatrix;

			CommandBuffer->pushConstants<glm::mat4>(
				DebugDrawPipelineLayout.get(),
				vk::ShaderStageFlagBits::eAllGraphics, 0, {ViewProjMatrix});

			CommandBuffer->bindVertexBuffers(
				0, {VertexBuffer.get(), LightmapVertexBuffer.get()}, {0, 0});
			CommandBuffer->bindIndexBuffer(
				IndexBuffer.get(), 0, vk::IndexType::eUint16);

			for( std::size_t i = 0; i < VertexIndexOffsets.size(); ++i )
			{
				Vulkan::InsertDebugLabel(
					CommandBuffer.get(), {0.5, 0.5, 0.5, 1.0}, "BSP Draw: %zu",
					i);
				if( LightmapSets.count(LightmapIndex[i].BitmapID)
					&& LightmapSets.at(LightmapIndex[i].BitmapID)
						   .count(LightmapIndex[i].BitmapIndex) )
				{
					CommandBuffer->bindDescriptorSets(
						vk::PipelineBindPoint::eGraphics,
						DebugDrawPipelineLayout.get(), 0,
						{LightmapSets.at(LightmapIndex[i].BitmapID)
							 .at(LightmapIndex[i].BitmapIndex)
							 .get()},
						{});
				}
				CommandBuffer->drawIndexed(
					IndexCounts[i], 1, IndexOffsets[i], VertexIndexOffsets[i],
					0);
			}

			// CommandBuffer->bindPipeline(
			// 	vk::PipelineBindPoint::eGraphics, UnlitDrawPipeline.get());
			// CommandBuffer->pushConstants<glm::vec4>(
			// 	UnlitDrawPipelineLayout.get(),
			// 	vk::ShaderStageFlagBits::eAllGraphics, sizeof(glm::f32mat4),
			// 	{glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)});
			// for( std::size_t i = 0; i < VertexIndexOffsets.size(); ++i )
			// {
			// 	Vulkan::InsertDebugLabel(
			// 		CommandBuffer.get(), {0.5, 0.5, 0.5, 1.0}, "BSP Draw: %zu",
			// 		i);
			// 	CommandBuffer->drawIndexed(
			// 		IndexCounts[i], 1, IndexOffsets[i], VertexIndexOffsets[i],
			// 		0);
			// }

			CommandBuffer->endRenderPass();
		}

		// Wait for image data to be ready
		{
			Vulkan::DebugLabelScope DebugCopyScope(
				CommandBuffer.get(), {1.0, 1.0, 0.0, 1.0},
				"Upload framebuffer to staging buffer");
			CommandBuffer->pipelineBarrier(
				vk::PipelineStageFlagBits::eColorAttachmentOutput,
				vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(), {},
				{},
				{// Source Image
				 vk::ImageMemoryBarrier(
					 vk::AccessFlagBits::eColorAttachmentWrite,
					 vk::AccessFlagBits::eTransferRead,
					 vk::ImageLayout::eTransferSrcOptimal,
					 vk::ImageLayout::eTransferSrcOptimal,
					 VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					 RenderImage.get(),
					 vk::ImageSubresourceRange(
						 vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1))});
			CommandBuffer->copyImageToBuffer(
				RenderImage.get(), vk::ImageLayout::eTransferSrcOptimal,
				StagingBuffer.get(),
				{vk::BufferImageCopy(
					0, RenderSize.x, RenderSize.y,
					vk::ImageSubresourceLayers(
						vk::ImageAspectFlagBits::eColor, 0, 0, 1),
					vk::Offset3D(),
					vk::Extent3D(RenderSize.x, RenderSize.y, 1))});
		}
	}

	if( auto EndResult = CommandBuffer->end();
		EndResult != vk::Result::eSuccess )
	{
		std::fprintf(
			stderr, "Error ending command buffer: %s\n",
			vk::to_string(EndResult).c_str());
		return EXIT_FAILURE;
	}

	// Submit work
	vk::UniqueFence Fence = {};
	if( auto CreateResult = Device->createFenceUnique({});
		CreateResult.result == vk::Result::eSuccess )
	{
		Fence = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating fence: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return EXIT_FAILURE;
	}

	vk::SubmitInfo SubmitInfo     = {};
	SubmitInfo.commandBufferCount = 1;
	SubmitInfo.pCommandBuffers    = &CommandBuffer.get();

	if( auto SubmitResult = RenderQueue.submit(SubmitInfo, Fence.get());
		SubmitResult != vk::Result::eSuccess )
	{
		std::fprintf(
			stderr, "Error submitting command buffer: %s\n",
			vk::to_string(SubmitResult).c_str());
		return EXIT_FAILURE;
	}

	// Wait for it
	if( auto WaitResult = Device->waitForFences(Fence.get(), true, ~0ULL);
		WaitResult != vk::Result::eSuccess )
	{
		std::fprintf(
			stderr, "Error waiting for fence: %s\n",
			vk::to_string(WaitResult).c_str());
		return EXIT_FAILURE;
	}

#ifdef CAPTURE
	if( rdoc_api )
		rdoc_api->EndFrameCapture(
			*(void**)(VkInstance)(*Instance.operator->()), NULL);
#endif

	stbi_write_png_compression_level = 0;
	stbi_write_png(
		("./" + CurPath.stem().string() + ".png").c_str(), RenderSize.x,
		RenderSize.y, 4, StagingBufferData.data(), 0);

	return EXIT_SUCCESS;
}

vk::UniqueDescriptorPool CreateMainDescriptorPool(vk::Device Device)
{
	constexpr std::size_t MaxDescriptorType = 128;

	const vk::DescriptorPoolSize PoolSizes[] = {
		{vk::DescriptorType::eStorageBuffer, MaxDescriptorType},
		{vk::DescriptorType::eSampler, MaxDescriptorType},
		{vk::DescriptorType::eSampledImage, MaxDescriptorType},
		{vk::DescriptorType::eCombinedImageSampler, MaxDescriptorType},
	};

	vk::DescriptorPoolCreateInfo DescriptorPoolInfo{};
	DescriptorPoolInfo.flags
		= vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	DescriptorPoolInfo.poolSizeCount = std::extent<decltype(PoolSizes)>();
	DescriptorPoolInfo.pPoolSizes    = PoolSizes;
	DescriptorPoolInfo.maxSets       = 0xFFFF;

	if( auto CreateResult
		= Device.createDescriptorPoolUnique(DescriptorPoolInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		return std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating descriptor pool: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}
}

vk::UniqueRenderPass
	CreateMainRenderPass(vk::Device Device, vk::SampleCountFlagBits SampleCount)
{
	vk::RenderPassCreateInfo RenderPassInfo = {};

	const vk::AttachmentDescription Attachments[] = {
		// Color Attachment
		// We just care about it storing its color data
		vk::AttachmentDescription(
			vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Srgb,
			vk::SampleCountFlagBits::e1, vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eStore, vk::AttachmentLoadOp::eDontCare,
			vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined,
			vk::ImageLayout::eTransferSrcOptimal),
		// Depth Attachment
		// Dont care about reading or storing it
		vk::AttachmentDescription(
			vk::AttachmentDescriptionFlags(), vk::Format::eD32Sfloat,
			SampleCount, vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eDontCare, vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined,
			vk::ImageLayout::eDepthStencilAttachmentOptimal),
		// Color Attachment(MSAA)
		// We just care about it storing its color data
		vk::AttachmentDescription(
			vk::AttachmentDescriptionFlags(), vk::Format::eR8G8B8A8Srgb,
			SampleCount, vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eDontCare, vk::AttachmentLoadOp::eDontCare,
			vk::AttachmentStoreOp::eDontCare, vk::ImageLayout::eUndefined,
			vk::ImageLayout::eColorAttachmentOptimal)};

	const vk::AttachmentReference AttachmentRefs[] = {
		vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal),
		vk::AttachmentReference(
			1, vk::ImageLayout::eDepthStencilAttachmentOptimal),
		vk::AttachmentReference(2, vk::ImageLayout::eColorAttachmentOptimal),
	};

	RenderPassInfo.attachmentCount = std::size(Attachments);
	RenderPassInfo.pAttachments    = Attachments;

	vk::SubpassDescription Subpasses[1] = {{}};

	// First subpass
	Subpasses[0].colorAttachmentCount    = 1;
	Subpasses[0].pColorAttachments       = &AttachmentRefs[2];
	Subpasses[0].pDepthStencilAttachment = &AttachmentRefs[1];
	Subpasses[0].pResolveAttachments     = &AttachmentRefs[0];

	RenderPassInfo.subpassCount = std::size(Subpasses);
	RenderPassInfo.pSubpasses   = Subpasses;

	const vk::SubpassDependency SubpassDependencies[] = {vk::SubpassDependency(
		VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eComputeShader,
		vk::PipelineStageFlagBits::eVertexInput,
		vk::AccessFlagBits::eShaderWrite,
		vk::AccessFlagBits::eVertexAttributeRead, vk::DependencyFlags())};

	RenderPassInfo.dependencyCount = std::size(SubpassDependencies);
	RenderPassInfo.pDependencies   = SubpassDependencies;

	if( auto CreateResult = Device.createRenderPassUnique(RenderPassInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		return std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating render pass: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}
}

vk::UniqueFramebuffer CreateMainFrameBuffer(
	vk::Device Device, vk::ImageView Color, vk::ImageView DepthAA,
	vk::ImageView ColorAA, glm::uvec2 ImageSize, vk::RenderPass RenderPass)
{
	vk::FramebufferCreateInfo FramebufferInfo = {};

	FramebufferInfo.width      = ImageSize.x;
	FramebufferInfo.height     = ImageSize.y;
	FramebufferInfo.layers     = 1;
	FramebufferInfo.renderPass = RenderPass;

	const vk::ImageView Attachments[] = {Color, DepthAA, ColorAA};
	FramebufferInfo.attachmentCount   = std::size(Attachments);
	FramebufferInfo.pAttachments      = Attachments;

	if( auto CreateResult = Device.createFramebufferUnique(FramebufferInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		return std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating framebuffer: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}
}

std::tuple<
	vk::UniquePipeline, vk::UniquePipelineLayout, vk::UniqueDescriptorSetLayout>
	CreateGraphicsPipeline(
		vk::Device                                         Device,
		const std::vector<vk::PushConstantRange>&          PushConstants,
		const std::vector<vk::DescriptorSetLayoutBinding>& Bindings,
		vk::ShaderModule VertModule, vk::ShaderModule FragModule,
		vk::RenderPass RenderPass, vk::PolygonMode PolygonMode)
{
	// Create Descriptor layout
	vk::DescriptorSetLayoutCreateInfo GraphicsDescriptorLayoutInfo{};
	GraphicsDescriptorLayoutInfo.bindingCount = Bindings.size();
	GraphicsDescriptorLayoutInfo.pBindings    = Bindings.data();

	vk::UniqueDescriptorSetLayout GraphicsDescriptorLayout = {};
	if( auto CreateResult
		= Device.createDescriptorSetLayoutUnique(GraphicsDescriptorLayoutInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		GraphicsDescriptorLayout = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating descriptor set layout: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}

	// Create Pipeline Layout
	vk::PipelineLayoutCreateInfo GraphicsPipelineLayoutInfo{};
	GraphicsPipelineLayoutInfo.setLayoutCount = 1;
	GraphicsPipelineLayoutInfo.pSetLayouts    = &GraphicsDescriptorLayout.get();
	GraphicsPipelineLayoutInfo.pushConstantRangeCount = PushConstants.size();
	GraphicsPipelineLayoutInfo.pPushConstantRanges    = PushConstants.data();

	vk::UniquePipelineLayout GraphicsPipelineLayout = {};
	if( auto CreateResult
		= Device.createPipelineLayoutUnique(GraphicsPipelineLayoutInfo);
		CreateResult.result == vk::Result::eSuccess )
	{
		GraphicsPipelineLayout = std::move(CreateResult.value);
	}
	else
	{
		std::fprintf(
			stderr, "Error creating pipeline layout: %s\n",
			vk::to_string(CreateResult.result).c_str());
		return {};
	}

	// Describe the stage and entry point of each shader
	const vk::PipelineShaderStageCreateInfo ShaderStagesInfo[2] = {
		vk::PipelineShaderStageCreateInfo(
			{},                               // Flags
			vk::ShaderStageFlagBits::eVertex, // Shader Stage
			VertModule,                       // Shader Module
			"main", // Shader entry point function name
			{}      // Shader specialization info
			),
		vk::PipelineShaderStageCreateInfo(
			{},                                 // Flags
			vk::ShaderStageFlagBits::eFragment, // Shader Stage
			FragModule,                         // Shader Module
			"main", // Shader entry point function name
			{}      // Shader specialization info
			),
	};

	vk::PipelineVertexInputStateCreateInfo VertexInputState = {};

	static std::array<vk::VertexInputBindingDescription, 2>
		VertexBindingDescriptions = {};

	VertexBindingDescriptions[0].binding   = 0;
	VertexBindingDescriptions[0].stride    = sizeof(Blam::Vertex);
	VertexBindingDescriptions[0].inputRate = vk::VertexInputRate::eVertex;

	VertexBindingDescriptions[1].binding   = 1;
	VertexBindingDescriptions[1].stride    = sizeof(Blam::LightmapVertex);
	VertexBindingDescriptions[1].inputRate = vk::VertexInputRate::eVertex;

	VertexInputState.vertexBindingDescriptionCount
		= std::size(VertexBindingDescriptions);
	VertexInputState.pVertexBindingDescriptions
		= VertexBindingDescriptions.data();

	static std::array<vk::VertexInputAttributeDescription, 7>
		AttributeDescriptions = {};
	// Position
	AttributeDescriptions[0].binding  = 0;
	AttributeDescriptions[0].location = 0;
	AttributeDescriptions[0].format   = vk::Format::eR32G32B32Sfloat;
	AttributeDescriptions[0].offset   = offsetof(Blam::Vertex, Position);
	// Normal
	AttributeDescriptions[1].binding  = 0;
	AttributeDescriptions[1].location = 1;
	AttributeDescriptions[1].format   = vk::Format::eR32G32B32Sfloat;
	AttributeDescriptions[1].offset   = offsetof(Blam::Vertex, Normal);
	// Binormal
	AttributeDescriptions[2].binding  = 0;
	AttributeDescriptions[2].location = 2;
	AttributeDescriptions[2].format   = vk::Format::eR32G32B32Sfloat;
	AttributeDescriptions[2].offset   = offsetof(Blam::Vertex, Binormal);
	// Tangent
	AttributeDescriptions[3].binding  = 0;
	AttributeDescriptions[3].location = 3;
	AttributeDescriptions[3].format   = vk::Format::eR32G32B32Sfloat;
	AttributeDescriptions[3].offset   = offsetof(Blam::Vertex, Tangent);
	// UV
	AttributeDescriptions[4].binding  = 0;
	AttributeDescriptions[4].location = 4;
	AttributeDescriptions[4].format   = vk::Format::eR32G32Sfloat;
	AttributeDescriptions[4].offset   = offsetof(Blam::Vertex, UV);

	// Normal-Lightmap
	AttributeDescriptions[5].binding  = 1;
	AttributeDescriptions[5].location = 5;
	AttributeDescriptions[5].format   = vk::Format::eR32G32B32Sfloat;
	AttributeDescriptions[5].offset   = offsetof(Blam::LightmapVertex, Normal);
	// UV-Lightmap
	AttributeDescriptions[6].binding  = 1;
	AttributeDescriptions[6].location = 6;
	AttributeDescriptions[6].format   = vk::Format::eR32G32Sfloat;
	AttributeDescriptions[6].offset   = offsetof(Blam::LightmapVertex, UV);

	VertexInputState.vertexAttributeDescriptionCount
		= AttributeDescriptions.size();
	VertexInputState.pVertexAttributeDescriptions
		= AttributeDescriptions.data();

	vk::PipelineInputAssemblyStateCreateInfo InputAssemblyState = {};
	InputAssemblyState.topology = vk::PrimitiveTopology::eTriangleList;
	InputAssemblyState.primitiveRestartEnable = false;

	vk::PipelineViewportStateCreateInfo ViewportState = {};
	static const vk::Viewport DefaultViewport = {0, 0, 16, 16, 0.0f, 1.0f};
	static const vk::Rect2D   DefaultScissor  = {{0, 0}, {16, 16}};
	ViewportState.viewportCount               = 1;
	ViewportState.pViewports                  = &DefaultViewport;
	ViewportState.scissorCount                = 1;
	ViewportState.pScissors                   = &DefaultScissor;

	vk::PipelineRasterizationStateCreateInfo RasterizationState = {};
	RasterizationState.depthClampEnable                         = false;
	RasterizationState.rasterizerDiscardEnable                  = false;
	RasterizationState.polygonMode                              = PolygonMode;
	RasterizationState.cullMode        = vk::CullModeFlagBits::eBack;
	RasterizationState.frontFace       = vk::FrontFace::eCounterClockwise;
	RasterizationState.depthBiasEnable = false;
	RasterizationState.depthBiasConstantFactor = 0.0f;
	RasterizationState.depthBiasClamp          = 0.0f;
	RasterizationState.depthBiasSlopeFactor    = 0.0;
	RasterizationState.lineWidth               = 1.0f;

	vk::PipelineMultisampleStateCreateInfo MultisampleState = {};
	MultisampleState.rasterizationSamples                   = RenderSamples;
	MultisampleState.sampleShadingEnable                    = true;
	MultisampleState.minSampleShading                       = 1.0f;
	MultisampleState.pSampleMask                            = nullptr;
	MultisampleState.alphaToCoverageEnable                  = false;
	MultisampleState.alphaToOneEnable                       = false;

	vk::PipelineDepthStencilStateCreateInfo DepthStencilState = {};
	DepthStencilState.depthTestEnable                         = true;
	DepthStencilState.depthWriteEnable                        = true;
	DepthStencilState.depthCompareOp        = vk::CompareOp::eLessOrEqual;
	DepthStencilState.depthBoundsTestEnable = false;
	DepthStencilState.stencilTestEnable     = false;
	DepthStencilState.front                 = vk::StencilOp::eKeep;
	DepthStencilState.back                  = vk::StencilOp::eKeep;
	DepthStencilState.minDepthBounds        = 0.0f;
	DepthStencilState.maxDepthBounds        = 1.0f;

	vk::PipelineColorBlendStateCreateInfo ColorBlendState = {};
	ColorBlendState.logicOpEnable                         = false;
	ColorBlendState.logicOp                               = vk::LogicOp::eClear;
	ColorBlendState.attachmentCount                       = 1;

	vk::PipelineColorBlendAttachmentState BlendAttachmentState = {};
	BlendAttachmentState.blendEnable                           = false;
	BlendAttachmentState.srcColorBlendFactor = vk::BlendFactor::eZero;
	BlendAttachmentState.dstColorBlendFactor = vk::BlendFactor::eZero;
	BlendAttachmentState.colorBlendOp        = vk::BlendOp::eAdd;
	BlendAttachmentState.srcAlphaBlendFactor = vk::BlendFactor::eZero;
	BlendAttachmentState.dstAlphaBlendFactor = vk::BlendFactor::eZero;
	BlendAttachmentState.alphaBlendOp        = vk::BlendOp::eAdd;
	BlendAttachmentState.colorWriteMask
		= vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
		| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	ColorBlendState.pAttachments = &BlendAttachmentState;

	vk::PipelineDynamicStateCreateInfo DynamicState = {};
	vk::DynamicState                   DynamicStates[]
		= {// The viewport and scissor of the framebuffer will be dynamic at
		   // run-time
		   // so we definately add these
		   vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	DynamicState.dynamicStateCount = std::size(DynamicStates);
	DynamicState.pDynamicStates    = DynamicStates;

	vk::GraphicsPipelineCreateInfo RenderPipelineInfo = {};
	RenderPipelineInfo.stageCount                     = 2; // Vert + Frag stages
	RenderPipelineInfo.pStages                        = ShaderStagesInfo;
	RenderPipelineInfo.pVertexInputState              = &VertexInputState;
	RenderPipelineInfo.pInputAssemblyState            = &InputAssemblyState;
	RenderPipelineInfo.pViewportState                 = &ViewportState;
	RenderPipelineInfo.pRasterizationState            = &RasterizationState;
	RenderPipelineInfo.pMultisampleState              = &MultisampleState;
	RenderPipelineInfo.pDepthStencilState             = &DepthStencilState;
	RenderPipelineInfo.pColorBlendState               = &ColorBlendState;
	RenderPipelineInfo.pDynamicState                  = &DynamicState;
	RenderPipelineInfo.subpass                        = 0;
	RenderPipelineInfo.renderPass                     = RenderPass;
	RenderPipelineInfo.layout = GraphicsPipelineLayout.get();

	// Create Pipeline
	vk::UniquePipeline Pipeline
		= Device.createGraphicsPipelineUnique({}, RenderPipelineInfo).value;
	return std::make_tuple(
		std::move(Pipeline), std::move(GraphicsPipelineLayout),
		std::move(GraphicsDescriptorLayout));
}