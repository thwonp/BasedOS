# BasedOS packages of its own go in package/<name>/.
include $(sort $(wildcard $(BR2_EXTERNAL_BASEDOS_PATH)/package/*/*.mk))

# ncurses with its terminfo library apart (libtinfo.so.6), as ROCKNIX and most distributions
# build it: spruce's card binaries (bin64/rnano, flip/lib/libmp3lame) link libtinfo.so.6, and
# Buildroot's ncurses has no option for it. Appended here, after ncurses.mk; the configure step
# reads it when it runs.
NCURSES_CONF_OPTS += --with-termlib=tinfo

# LAME: the library only (libmp3lame, which 32-bit ports load). Its command-line encoder links
# -lncurses for terminfo calls that are in libtinfo with the split above, and fails to link.
LAME_CONF_OPTS += --disable-frontend

# The 32-bit root (basedos_gkd350hultra_armhf_defconfig): its libraries go to /usr/lib32 of the
# system layer, beside the 64-bit ones in /usr/lib (docs/lib32.md). glibc reads configparms from
# its build folder (written here after configure, before the build):
# - default-rpath is the loader's built-in search path (only its trusted-dirs list uses it):
#   /usr/lib32 alone, so the 32-bit loader never opens the 64-bit libraries of the same names.
#   `override`, since Makeconfig sets it again after reading configparms.
# - gconvdir is where iconv() loads its modules: /usr/lib32/gconv, not the 64-bit /usr/lib/gconv.
#   Buildroot's own copy of them looks only in /usr/lib/gconv, so they are copied below, into
#   this root's /usr/lib/gconv (the system layer's /usr/lib32/gconv).
ifeq ($(BR2_arm),y)
define BASEDOS_GLIBC_LIB32
	printf 'override default-rpath = /usr/lib32\ngconvdir = /usr/lib32/gconv\n' > $(@D)/build/configparms
endef
GLIBC_POST_CONFIGURE_HOOKS += BASEDOS_GLIBC_LIB32

define BASEDOS_LIB32_GCONV
	rm -rf $(TARGET_DIR)/usr/lib/gconv
	mkdir -p $(TARGET_DIR)/usr/lib/gconv
	$(INSTALL) -m 0644 $(STAGING_DIR)/usr/lib32/gconv/gconv-modules $(STAGING_DIR)/usr/lib32/gconv/*.so \
		$(TARGET_DIR)/usr/lib/gconv/
	if [ -d $(STAGING_DIR)/usr/lib32/gconv/gconv-modules.d ]; then \
		cp -a $(STAGING_DIR)/usr/lib32/gconv/gconv-modules.d $(TARGET_DIR)/usr/lib/gconv/; \
	fi
endef
TARGET_FINALIZE_HOOKS += BASEDOS_LIB32_GCONV
endif

# PipeWire's ALSA config: pcm.pipewire stays (50-pipewire.conf), but not 99-pipewire-default.conf,
# which would send every program's "default" device to PipeWire whether it runs or not. A frontend
# that runs PipeWire points "default" at it in its own ~/.asoundrc.
define BASEDOS_PIPEWIRE_ALSA_DEFAULT_OFF
	rm -f $(TARGET_DIR)/usr/share/alsa/alsa.conf.d/99-pipewire-default.conf
endef
PIPEWIRE_POST_INSTALL_TARGET_HOOKS += BASEDOS_PIPEWIRE_ALSA_DEFAULT_OFF

# pactl and libpulse find libpulsecommon in /usr/lib/pulseaudio by their RUNPATH; the system
# layer's library closure (tools/harvest.py) looks in /usr/lib only. A link there.
define BASEDOS_PULSECOMMON_LINK
	for f in $(TARGET_DIR)/usr/lib/pulseaudio/libpulsecommon-*.so; do \
		ln -sf pulseaudio/$${f##*/} $(TARGET_DIR)/usr/lib/$${f##*/}; \
	done
endef
PULSEAUDIO_POST_INSTALL_TARGET_HOOKS += BASEDOS_PULSECOMMON_LINK
