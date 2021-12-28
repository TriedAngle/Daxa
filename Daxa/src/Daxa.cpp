// Daxa.cpp : Defines the functions for the static library.
//

#include "framework.h"

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>
#include "glslang/Public/ShaderLang.h"

#include "gpu/Instance.hpp"

namespace daxa {

	void initialize() {
		if (glfwInit() != GLFW_TRUE)
		{
			printf("error: could not initialize GLFW3!\n");
			exit(-1);
		}
		gpu::instance = std::make_unique<gpu::Instance>();
		glslang::InitializeProcess();
	}

	void cleanup() {
		glslang::FinalizeProcess();
		gpu::instance.reset();
	}
}