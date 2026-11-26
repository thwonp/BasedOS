################################################################################
#
# basedos-vkrotate: VK_LAYER_BASEDOS_rotate, turns VK_KHR_display apps' frames onto the portrait panel
#
################################################################################

BASEDOS_VKROTATE_VERSION = 1
BASEDOS_VKROTATE_SITE = $(BR2_EXTERNAL_BASEDOS_PATH)/package/basedos-vkrotate/src
BASEDOS_VKROTATE_SITE_METHOD = local
BASEDOS_VKROTATE_LICENSE = MIT
BASEDOS_VKROTATE_DEPENDENCIES = vulkan-headers

# The shaders are SPIR-V in spirv.h, made by src/gen-spirv.py (no shader compiler in this build).
define BASEDOS_VKROTATE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Wno-unused-parameter -fPIC -shared \
		-fvisibility=hidden -Wl,-Bsymbolic -Wl,-z,defs $(TARGET_LDFLAGS) \
		-o $(@D)/libVkLayer_basedos_rotate.so $(@D)/vkrotate.c -lpthread
endef

define BASEDOS_VKROTATE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libVkLayer_basedos_rotate.so $(TARGET_DIR)/usr/lib/libVkLayer_basedos_rotate.so
	$(INSTALL) -D -m 0644 $(@D)/VkLayer_basedos_rotate.json \
		$(TARGET_DIR)/usr/share/vulkan/implicit_layer.d/VkLayer_basedos_rotate.json
endef

$(eval $(generic-package))
