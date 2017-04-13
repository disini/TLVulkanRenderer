 #pragma once
#include "VulkanRenderer.h"
#include "VulkanBuffer.h"
#include "renderer/Film.h"
#include <thread>
#include <queue>
#include <array>

class VulkanCPURaytracer : public VulkanRenderer
{

public:
	VulkanCPURaytracer(
		GLFWwindow* window,
		Scene* scene,
		std::shared_ptr<std::map<string, string>> config
	);

	virtual ~VulkanCPURaytracer() final;

	// -----------
	// GRAPHICS PIPELINE
	// -----------
	void
	Prepare() final;

	VkResult
	PrepareGraphicsPipeline() final;

	VkResult
	PrepareVertexBuffers() final;

	VkResult
	PrepareUniforms() final;

	// --- Descriptor

	VkResult
	PrepareDescriptorPool() final;

	VkResult
	PrepareDescriptorLayouts() final;

	VkResult
	PrepareDescriptorSets() final;

	VkResult
	BuildCommandBuffers() final;

	void
	PrepareTextures() final;

	void
	Update() override final;

	void
	Render() override final;
	 
protected:

	// --- Command buffers

	struct Quad
	{
		std::vector<uint16_t> indices;
		std::vector<vec2> positions;
		std::vector<vec2> uvs;
	} m_quad;


protected:
	VulkanImage::Image m_stagingImage;
	VulkanImage::Image m_displayImage;
	VulkanBuffer::StorageBuffer m_quadUniform;

	Film m_film;

	std::array<std::thread, 16> m_threads;
	queue<Ray> m_raysQueue;

};
