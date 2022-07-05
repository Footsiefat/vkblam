#include <VkBlam/VkBlam.hpp>

namespace VkBlam
{

vk::ImageType BlamToVk(Blam::BitmapEntryType Value)
{
	switch( Value )
	{
	case Blam::BitmapEntryType::Texture2D:
		return vk::ImageType::e2D;
	case Blam::BitmapEntryType::Texture3D:
		return vk::ImageType::e3D;
	case Blam::BitmapEntryType::CubeMap:
		return vk::ImageType::e2D;
	case Blam::BitmapEntryType::White:
		return vk::ImageType::e2D;
	}
	return vk::ImageType::e2D;
}

vk::Format BlamToVk(Blam::BitmapEntryFormat Value)
{
	switch( Value )
	{
	case Blam::BitmapEntryFormat::A8:
		return vk::Format::eR8Unorm;
	case Blam::BitmapEntryFormat::Y8:
		return vk::Format::eR8Unorm;
	case Blam::BitmapEntryFormat::AY8:
		return vk::Format::eR8Unorm;
	case Blam::BitmapEntryFormat::A8Y8:
		return vk::Format::eR8G8Unorm;
	case Blam::BitmapEntryFormat::R5G6B5:
		return vk::Format::eR5G6B5UnormPack16;
	case Blam::BitmapEntryFormat::A1R5G5B5:
		return vk::Format::eA1R5G5B5UnormPack16;
	case Blam::BitmapEntryFormat::A4R4G4B4:
		return vk::Format::eA4R4G4B4UnormPack16EXT;
	case Blam::BitmapEntryFormat::X8R8G8B8:
		return vk::Format::eA8B8G8R8UnormPack32;
	case Blam::BitmapEntryFormat::A8R8G8B8:
		return vk::Format::eA8B8G8R8UnormPack32;
	case Blam::BitmapEntryFormat::DXT1:
		return vk::Format::eBc1RgbSrgbBlock;
	case Blam::BitmapEntryFormat::DXT2AND3:
		return vk::Format::eBc2SrgbBlock;
	case Blam::BitmapEntryFormat::DXT4AND5:
		return vk::Format::eBc3SrgbBlock;
	case Blam::BitmapEntryFormat::P8:
		return vk::Format::eR8Unorm;
	}
	return vk::Format::eUndefined;
}
} // namespace VkBlam