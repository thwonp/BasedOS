################################################################################
#
# luajit-lib: LuaJIT's shared library only (libluajit-5.1.so.2), for PortMaster's
# ports, beside Lua 5.4 (WirePlumber's). Buildroot's luajit is a luainterpreter
# provider and excludes lua; this one provides nothing and installs no headers.
#
################################################################################

LUAJIT_LIB_VERSION = a4f56a459a588ae768801074b46ba0adcfb49eb1
LUAJIT_LIB_SITE = $(call github,LuaJIT,LuaJIT,$(LUAJIT_LIB_VERSION))
LUAJIT_LIB_SOURCE = luajit-$(LUAJIT_LIB_VERSION).tar.gz
LUAJIT_LIB_DL_SUBDIR = luajit
LUAJIT_LIB_LICENSE = MIT
LUAJIT_LIB_LICENSE_FILES = COPYRIGHT
LUAJIT_LIB_INSTALL_STAGING = NO

define LUAJIT_LIB_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) PREFIX="/usr" \
		STATIC_CC="$(TARGET_CC)" DYNAMIC_CC="$(TARGET_CC) -fPIC" \
		TARGET_LD="$(TARGET_CC)" TARGET_AR="$(TARGET_AR) rcus" TARGET_STRIP=true \
		TARGET_CFLAGS="$(TARGET_CFLAGS)" TARGET_LDFLAGS="$(TARGET_LDFLAGS)" \
		HOST_CC="$(HOSTCC)" HOST_CFLAGS="$(HOST_CFLAGS)" HOST_LDFLAGS="$(HOST_LDFLAGS)" \
		BUILDMODE=dynamic -C $(@D) amalg
endef

define LUAJIT_LIB_INSTALL_TARGET_CMDS
	cp -a $(@D)/src/libluajit.so $(TARGET_DIR)/usr/lib/libluajit-5.1.so.2
endef

$(eval $(generic-package))
