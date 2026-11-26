/*
 * VK_LAYER_BASEDOS_rotate: Vulkan's counterpart of BasedOS's SDL2 rotation (tools/sdl2) for the
 * 350H Ultra, whose panel is 1440x1600 portrait and is held as 1600x1440 landscape.
 *
 * Apps that draw straight to the panel through VK_KHR_display (RetroArch's Vulkan driver, by its
 * khr_display context) see the panel as landscape: its display, modes, plane limits and surfaces are
 * reported with width and height swapped. A swapchain on such a surface is made at the panel's
 * real portrait size, and the app gets landscape images of its own to draw into; on present, one
 * draw turns the app's image into the real one (a fragment shader fetches, for each panel pixel,
 * the app pixel that belongs there), and that one is presented.
 *
 * Only displays taller than wide are turned, and only surfaces the app asks for landscape:
 * an app that asks for the panel's own portrait size gets it, untouched.
 *
 * Environment:
 *   BASEDOS_VK_ROTATE_DISABLE=1   the loader leaves the layer out (its manifest says so)
 *   BASEDOS_VK_ROTATE=cw|ccw      which way a landscape frame is turned onto the panel
 *                                 (default ccw: the 350H Ultra, as the SDL2 patch's RGA ROT_270)
 *   BASEDOS_VK_ROTATE_LOG=1       what the layer does, on stderr
 *   BASEDOS_VK_ROTATE_STATS=1     the turn's GPU time and the present's CPU time every 300 frames
 *
 * Copyright (c) 2026 theycallmeboxy. MIT License (the repository's LICENSE).
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "spirv.h"

#define LAYER_NAME "VK_LAYER_BASEDOS_rotate"
#define EXPORT __attribute__((visibility("default")))
#define STATS_EVERY 300

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int log_on = -1, stats_on = -1, turn_cw = -1;

static void settings(void)
{
	if (log_on >= 0)
		return;
	const char *v = getenv("BASEDOS_VK_ROTATE_LOG");
	log_on = v && *v && strcmp(v, "0") != 0;
	v = getenv("BASEDOS_VK_ROTATE_STATS");
	stats_on = v && *v && strcmp(v, "0") != 0;
	v = getenv("BASEDOS_VK_ROTATE");
	turn_cw = v && strcmp(v, "cw") == 0;
}

static void say(const char *fmt, ...)
{
	if (!log_on)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("[basedos-vk-rotate] ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void *key_of(const void *dispatchable)
{
	return *(void **)dispatchable;
}

/* ---- Instances and physical devices ---------------------------------------------------------- */

typedef struct inst {
	struct inst *next;
	void *key;
	VkInstance instance;
	PFN_vkGetInstanceProcAddr gipa;
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
	PFN_vkGetPhysicalDeviceDisplayPropertiesKHR GetPhysicalDeviceDisplayPropertiesKHR;
	PFN_vkGetPhysicalDeviceDisplayProperties2KHR GetPhysicalDeviceDisplayProperties2KHR;
	PFN_vkGetDisplayModePropertiesKHR GetDisplayModePropertiesKHR;
	PFN_vkGetDisplayModeProperties2KHR GetDisplayModeProperties2KHR;
	PFN_vkCreateDisplayModeKHR CreateDisplayModeKHR;
	PFN_vkGetDisplayPlaneCapabilitiesKHR GetDisplayPlaneCapabilitiesKHR;
	PFN_vkGetDisplayPlaneCapabilities2KHR GetDisplayPlaneCapabilities2KHR;
	PFN_vkCreateDisplayPlaneSurfaceKHR CreateDisplayPlaneSurfaceKHR;
	PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR;
} inst;

static inst *instances;

/* A physical device's instance: recorded when the devices are enumerated. */
typedef struct phys {
	struct phys *next;
	VkPhysicalDevice pd;
	inst *in;
} phys;

static phys *physicals;

static inst *inst_by_key(void *key)
{
	for (inst *i = instances; i; i = i->next)
		if (i->key == key)
			return i;
	return NULL;
}

static inst *inst_of_pd(VkPhysicalDevice pd)
{
	inst *found = NULL;
	pthread_mutex_lock(&lock);
	for (phys *p = physicals; p; p = p->next)
		if (p->pd == pd) {
			found = p->in;
			break;
		}
	if (!found)
		found = inst_by_key(key_of(pd));
	if (!found)
		found = instances;
	pthread_mutex_unlock(&lock);
	return found;
}

static inst *inst_of(VkInstance instance)
{
	pthread_mutex_lock(&lock);
	inst *i = inst_by_key(key_of(instance));
	pthread_mutex_unlock(&lock);
	return i;
}

/* ---- Displays, modes and surfaces that are turned ------------------------------------------- */

typedef struct handle_flag {
	struct handle_flag *next;
	uint64_t handle;
	bool turned;
} handle_flag;

static handle_flag *displays, *modes;

static int flag_get(handle_flag *list, uint64_t h)
{
	int r = -1;
	pthread_mutex_lock(&lock);
	for (handle_flag *f = list; f; f = f->next)
		if (f->handle == h) {
			r = f->turned;
			break;
		}
	pthread_mutex_unlock(&lock);
	return r;
}

static void flag_set(handle_flag **list, uint64_t h, bool turned)
{
	pthread_mutex_lock(&lock);
	for (handle_flag *f = *list; f; f = f->next)
		if (f->handle == h) {
			f->turned = turned;
			pthread_mutex_unlock(&lock);
			return;
		}
	handle_flag *f = calloc(1, sizeof(*f));
	if (f) {
		f->handle = h;
		f->turned = turned;
		f->next = *list;
		*list = f;
	}
	pthread_mutex_unlock(&lock);
}

typedef struct surface {
	struct surface *next;
	VkSurfaceKHR surface;
	VkExtent2D app; /* landscape, what the app sees */
} surface;

static surface *surfaces;

static bool surface_turned(VkSurfaceKHR s, VkExtent2D *app)
{
	bool r = false;
	pthread_mutex_lock(&lock);
	for (surface *f = surfaces; f; f = f->next)
		if (f->surface == s) {
			if (app)
				*app = f->app;
			r = true;
			break;
		}
	pthread_mutex_unlock(&lock);
	return r;
}

static void swap_extent(VkExtent2D *e)
{
	uint32_t t = e->width;
	e->width = e->height;
	e->height = t;
}

static void swap_offset(VkOffset2D *o)
{
	int32_t t = o->x;
	o->x = o->y;
	o->y = t;
}

static void swap_display_props(VkDisplayPropertiesKHR *p)
{
	swap_extent(&p->physicalDimensions);
	swap_extent(&p->physicalResolution);
}

static bool display_is_portrait(const VkDisplayPropertiesKHR *p)
{
	return p->physicalResolution.width < p->physicalResolution.height;
}

/* Whether a display is turned, asking the driver when the app hasn't listed the displays. */
static bool display_turned(inst *in, VkPhysicalDevice pd, VkDisplayKHR d)
{
	int t = flag_get(displays, (uint64_t)d);
	if (t >= 0 || !in->GetPhysicalDeviceDisplayPropertiesKHR)
		return t > 0;
	uint32_t n = 0;
	if (in->GetPhysicalDeviceDisplayPropertiesKHR(pd, &n, NULL) != VK_SUCCESS || !n)
		return false;
	VkDisplayPropertiesKHR *p = calloc(n, sizeof(*p));
	if (!p)
		return false;
	bool turned = false;
	if (in->GetPhysicalDeviceDisplayPropertiesKHR(pd, &n, p) >= 0)
		for (uint32_t i = 0; i < n; i++) {
			flag_set(&displays, (uint64_t)p[i].display, display_is_portrait(&p[i]));
			if (p[i].display == d)
				turned = display_is_portrait(&p[i]);
		}
	free(p);
	return turned;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice pd,
		uint32_t *count, VkDisplayPropertiesKHR *props)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetPhysicalDeviceDisplayPropertiesKHR(pd, count, props);
	if (props && r >= 0)
		for (uint32_t i = 0; i < *count; i++) {
			bool t = display_is_portrait(&props[i]);
			flag_set(&displays, (uint64_t)props[i].display, t);
			if (t) {
				say("display %s: %ux%u, shown as %ux%u", props[i].displayName ? props[i].displayName : "?",
				    props[i].physicalResolution.width, props[i].physicalResolution.height,
				    props[i].physicalResolution.height, props[i].physicalResolution.width);
				swap_display_props(&props[i]);
			}
		}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceDisplayProperties2KHR(VkPhysicalDevice pd,
		uint32_t *count, VkDisplayProperties2KHR *props)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetPhysicalDeviceDisplayProperties2KHR(pd, count, props);
	if (props && r >= 0)
		for (uint32_t i = 0; i < *count; i++) {
			VkDisplayPropertiesKHR *p = &props[i].displayProperties;
			bool t = display_is_portrait(p);
			flag_set(&displays, (uint64_t)p->display, t);
			if (t)
				swap_display_props(p);
		}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayModePropertiesKHR(VkPhysicalDevice pd,
		VkDisplayKHR display, uint32_t *count, VkDisplayModePropertiesKHR *props)
{
	inst *in = inst_of_pd(pd);
	bool t = display_turned(in, pd, display);
	VkResult r = in->GetDisplayModePropertiesKHR(pd, display, count, props);
	if (props && r >= 0)
		for (uint32_t i = 0; i < *count; i++) {
			flag_set(&modes, (uint64_t)props[i].displayMode, t);
			if (t)
				swap_extent(&props[i].parameters.visibleRegion);
		}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayModeProperties2KHR(VkPhysicalDevice pd,
		VkDisplayKHR display, uint32_t *count, VkDisplayModeProperties2KHR *props)
{
	inst *in = inst_of_pd(pd);
	bool t = display_turned(in, pd, display);
	VkResult r = in->GetDisplayModeProperties2KHR(pd, display, count, props);
	if (props && r >= 0)
		for (uint32_t i = 0; i < *count; i++) {
			VkDisplayModePropertiesKHR *p = &props[i].displayModeProperties;
			flag_set(&modes, (uint64_t)p->displayMode, t);
			if (t)
				swap_extent(&p->parameters.visibleRegion);
		}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDisplayModeKHR(VkPhysicalDevice pd, VkDisplayKHR display,
		const VkDisplayModeCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkDisplayModeKHR *mode)
{
	inst *in = inst_of_pd(pd);
	bool t = display_turned(in, pd, display);
	VkDisplayModeCreateInfoKHR ci = *info;
	if (t)
		swap_extent(&ci.parameters.visibleRegion);
	VkResult r = in->CreateDisplayModeKHR(pd, display, &ci, alloc, mode);
	if (r == VK_SUCCESS)
		flag_set(&modes, (uint64_t)*mode, t);
	return r;
}

static void swap_plane_caps(VkDisplayPlaneCapabilitiesKHR *c)
{
	swap_offset(&c->minSrcPosition);
	swap_offset(&c->maxSrcPosition);
	swap_extent(&c->minSrcExtent);
	swap_extent(&c->maxSrcExtent);
	swap_offset(&c->minDstPosition);
	swap_offset(&c->maxDstPosition);
	swap_extent(&c->minDstExtent);
	swap_extent(&c->maxDstExtent);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneCapabilitiesKHR(VkPhysicalDevice pd,
		VkDisplayModeKHR mode, uint32_t plane, VkDisplayPlaneCapabilitiesKHR *caps)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetDisplayPlaneCapabilitiesKHR(pd, mode, plane, caps);
	if (r == VK_SUCCESS && flag_get(modes, (uint64_t)mode) > 0)
		swap_plane_caps(caps);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetDisplayPlaneCapabilities2KHR(VkPhysicalDevice pd,
		const VkDisplayPlaneInfo2KHR *info, VkDisplayPlaneCapabilities2KHR *caps)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetDisplayPlaneCapabilities2KHR(pd, info, caps);
	if (r == VK_SUCCESS && flag_get(modes, (uint64_t)info->mode) > 0)
		swap_plane_caps(&caps->capabilities);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDisplayPlaneSurfaceKHR(VkInstance instance,
		const VkDisplaySurfaceCreateInfoKHR *info, const VkAllocationCallbacks *alloc, VkSurfaceKHR *out)
{
	inst *in = inst_of(instance);
	VkDisplaySurfaceCreateInfoKHR ci = *info;
	bool t = flag_get(modes, (uint64_t)info->displayMode) > 0 && ci.imageExtent.width > ci.imageExtent.height;
	if (t)
		swap_extent(&ci.imageExtent);
	VkResult r = in->CreateDisplayPlaneSurfaceKHR(instance, &ci, alloc, out);
	if (r == VK_SUCCESS && t) {
		surface *s = calloc(1, sizeof(*s));
		if (s) {
			s->surface = *out;
			s->app = info->imageExtent;
			pthread_mutex_lock(&lock);
			s->next = surfaces;
			surfaces = s;
			pthread_mutex_unlock(&lock);
		}
		say("surface %ux%u on the panel's %ux%u: turned %s", info->imageExtent.width,
		    info->imageExtent.height, ci.imageExtent.width, ci.imageExtent.height, turn_cw ? "cw" : "ccw");
	}
	return r;
}

static VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR s,
		const VkAllocationCallbacks *alloc)
{
	inst *in = inst_of(instance);
	pthread_mutex_lock(&lock);
	for (surface **p = &surfaces; *p; p = &(*p)->next)
		if ((*p)->surface == s) {
			surface *dead = *p;
			*p = dead->next;
			free(dead);
			break;
		}
	pthread_mutex_unlock(&lock);
	in->DestroySurfaceKHR(instance, s, alloc);
}

static void swap_surface_caps(VkSurfaceCapabilitiesKHR *c)
{
	if (c->currentExtent.width != 0xFFFFFFFFu)
		swap_extent(&c->currentExtent);
	swap_extent(&c->minImageExtent);
	swap_extent(&c->maxImageExtent);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd,
		VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *caps)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetPhysicalDeviceSurfaceCapabilitiesKHR(pd, s, caps);
	if (r == VK_SUCCESS && surface_turned(s, NULL))
		swap_surface_caps(caps);
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd,
		const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
	inst *in = inst_of_pd(pd);
	VkResult r = in->GetPhysicalDeviceSurfaceCapabilities2KHR(pd, info, caps);
	if (r == VK_SUCCESS && surface_turned(info->surface, NULL))
		swap_surface_caps(&caps->surfaceCapabilities);
	return r;
}

/* ---- Devices --------------------------------------------------------------------------------- */

typedef struct dev {
	struct dev *next;
	void *key;
	VkDevice device;
	VkPhysicalDevice pd;
	inst *in;
	PFN_vkSetDeviceLoaderData set_loader_data;
	float timestamp_period; /* ns per tick */
	VkPhysicalDeviceMemoryProperties mem;
	struct { VkQueue queue; uint32_t family; } queues[16];
	uint32_t nqueues;
#define D(name) PFN_vk##name name;
	D(GetDeviceProcAddr) D(DestroyDevice) D(GetDeviceQueue) D(GetDeviceQueue2) D(DeviceWaitIdle)
	D(CreateSwapchainKHR) D(DestroySwapchainKHR) D(GetSwapchainImagesKHR) D(AcquireNextImageKHR)
	D(AcquireNextImage2KHR) D(QueuePresentKHR) D(QueueSubmit)
	D(CreateImage) D(DestroyImage) D(GetImageMemoryRequirements) D(AllocateMemory) D(FreeMemory)
	D(BindImageMemory) D(CreateImageView) D(DestroyImageView) D(CreateRenderPass) D(DestroyRenderPass)
	D(CreateFramebuffer) D(DestroyFramebuffer) D(CreateShaderModule) D(DestroyShaderModule)
	D(CreateDescriptorSetLayout) D(DestroyDescriptorSetLayout) D(CreateDescriptorPool)
	D(DestroyDescriptorPool) D(AllocateDescriptorSets) D(UpdateDescriptorSets) D(CreatePipelineLayout)
	D(DestroyPipelineLayout) D(CreateGraphicsPipelines) D(DestroyPipeline) D(CreateCommandPool)
	D(DestroyCommandPool) D(AllocateCommandBuffers) D(BeginCommandBuffer) D(EndCommandBuffer)
	D(CmdPipelineBarrier) D(CmdBeginRenderPass) D(CmdEndRenderPass) D(CmdBindPipeline)
	D(CmdBindDescriptorSets) D(CmdPushConstants) D(CmdDraw) D(CmdResetQueryPool) D(CmdWriteTimestamp)
	D(CreateFence) D(DestroyFence) D(WaitForFences) D(ResetFences) D(CreateSemaphore) D(DestroySemaphore)
	D(CreateQueryPool) D(DestroyQueryPool) D(GetQueryPoolResults)
#undef D
} dev;

static dev *devices;

static dev *dev_of(const void *dispatchable)
{
	void *key = key_of(dispatchable);
	pthread_mutex_lock(&lock);
	dev *found = NULL;
	for (dev *d = devices; d; d = d->next)
		if (d->key == key) {
			found = d;
			break;
		}
	pthread_mutex_unlock(&lock);
	return found;
}

static uint32_t queue_family(dev *d, VkQueue q)
{
	for (uint32_t i = 0; i < d->nqueues; i++)
		if (d->queues[i].queue == q)
			return d->queues[i].family;
	return 0;
}

static void note_queue(dev *d, VkQueue q, uint32_t family)
{
	pthread_mutex_lock(&lock);
	bool known = false;
	for (uint32_t i = 0; i < d->nqueues; i++)
		known |= d->queues[i].queue == q;
	if (!known && d->nqueues < 16) {
		d->queues[d->nqueues].queue = q;
		d->queues[d->nqueues].family = family;
		d->nqueues++;
	}
	pthread_mutex_unlock(&lock);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *q)
{
	dev *d = dev_of(device);
	d->GetDeviceQueue(device, family, index, q);
	if (*q)
		note_queue(d, *q, family);
}

static VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
	dev *d = dev_of(device);
	d->GetDeviceQueue2(device, info, q);
	if (*q)
		note_queue(d, *q, info->queueFamilyIndex);
}

/* ---- Swapchains on turned surfaces ----------------------------------------------------------- */

typedef struct chain {
	struct chain *next;
	VkSwapchainKHR swapchain; /* the real one; the app holds the same handle */
	dev *d;
	VkExtent2D app, real;
	VkFormat format;
	uint32_t count;
	VkImage *real_images;
	VkImage *app_images;
	VkDeviceMemory *app_memory;
	VkImageView *app_views, *real_views;
	VkFramebuffer *framebuffers;
	VkRenderPass render_pass;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool pool;
	VkDescriptorSet *sets;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkCommandPool cmd_pool; /* made at the first present, for the presenting queue's family */
	VkCommandBuffer *cmds;
	VkFence *fences;
	VkSemaphore *done;
	bool *submitted;
	VkQueryPool queries;
	/* statistics */
	uint32_t frames, gpu_samples;
	double gpu_ms, cpu_ms;
} chain;

static chain *chains;

static chain *chain_of(VkSwapchainKHR s)
{
	chain *found = NULL;
	pthread_mutex_lock(&lock);
	for (chain *c = chains; c; c = c->next)
		if (c->swapchain == s) {
			found = c;
			break;
		}
	pthread_mutex_unlock(&lock);
	return found;
}

static void chain_free(chain *c)
{
	dev *d = c->d;
	VkDevice dv = d->device;
	if (c->fences) {
		for (uint32_t i = 0; i < c->count; i++)
			if (c->fences[i] && c->submitted[i])
				d->WaitForFences(dv, 1, &c->fences[i], VK_TRUE, UINT64_MAX);
	}
	for (uint32_t i = 0; i < c->count; i++) {
		if (c->fences && c->fences[i]) d->DestroyFence(dv, c->fences[i], NULL);
		if (c->done && c->done[i]) d->DestroySemaphore(dv, c->done[i], NULL);
		if (c->framebuffers && c->framebuffers[i]) d->DestroyFramebuffer(dv, c->framebuffers[i], NULL);
		if (c->real_views && c->real_views[i]) d->DestroyImageView(dv, c->real_views[i], NULL);
		if (c->app_views && c->app_views[i]) d->DestroyImageView(dv, c->app_views[i], NULL);
		if (c->app_images && c->app_images[i]) d->DestroyImage(dv, c->app_images[i], NULL);
		if (c->app_memory && c->app_memory[i]) d->FreeMemory(dv, c->app_memory[i], NULL);
	}
	if (c->queries) d->DestroyQueryPool(dv, c->queries, NULL);
	if (c->cmd_pool) d->DestroyCommandPool(dv, c->cmd_pool, NULL);
	if (c->pipeline) d->DestroyPipeline(dv, c->pipeline, NULL);
	if (c->pipeline_layout) d->DestroyPipelineLayout(dv, c->pipeline_layout, NULL);
	if (c->pool) d->DestroyDescriptorPool(dv, c->pool, NULL);
	if (c->set_layout) d->DestroyDescriptorSetLayout(dv, c->set_layout, NULL);
	if (c->render_pass) d->DestroyRenderPass(dv, c->render_pass, NULL);
	free(c->real_images); free(c->app_images); free(c->app_memory); free(c->app_views);
	free(c->real_views); free(c->framebuffers); free(c->sets); free(c->cmds); free(c->fences);
	free(c->done); free(c->submitted);
	free(c);
}

static int memory_type(dev *d, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (d->mem.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
		if (bits & (1u << i))
			return (int)i;
	return -1;
}

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
	say("%s failed: %d", #call, r_); return r_; } } while (0)

/* Everything a turned swapchain needs but the command buffers (which need the presenting queue). */
static VkResult chain_build(chain *c, const VkSwapchainCreateInfoKHR *info)
{
	dev *d = c->d;
	VkDevice dv = d->device;
	uint32_t n = c->count;
	c->app_images = calloc(n, sizeof(VkImage));
	c->app_memory = calloc(n, sizeof(VkDeviceMemory));
	c->app_views = calloc(n, sizeof(VkImageView));
	c->real_views = calloc(n, sizeof(VkImageView));
	c->framebuffers = calloc(n, sizeof(VkFramebuffer));
	c->sets = calloc(n, sizeof(VkDescriptorSet));
	c->cmds = calloc(n, sizeof(VkCommandBuffer));
	c->fences = calloc(n, sizeof(VkFence));
	c->done = calloc(n, sizeof(VkSemaphore));
	c->submitted = calloc(n, sizeof(bool));
	if (!c->app_images || !c->app_memory || !c->app_views || !c->real_views || !c->framebuffers ||
	    !c->sets || !c->cmds || !c->fences || !c->done || !c->submitted)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The images the app draws into: as it asked for the swapchain's, landscape, and sampled. */
	VkImageCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = info->imageFormat,
		.extent = { c->app.width, c->app.height, 1 },
		.mipLevels = 1,
		.arrayLayers = info->imageArrayLayers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = info->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = info->imageSharingMode,
		.queueFamilyIndexCount = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? info->queueFamilyIndexCount : 0,
		.pQueueFamilyIndices = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? info->pQueueFamilyIndices : NULL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
		ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	for (uint32_t i = 0; i < n; i++) {
		CHECK(d->CreateImage(dv, &ici, NULL, &c->app_images[i]));
		VkMemoryRequirements req;
		d->GetImageMemoryRequirements(dv, c->app_images[i], &req);
		int type = memory_type(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type < 0)
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
		CHECK(d->AllocateMemory(dv, &mai, NULL, &c->app_memory[i]));
		CHECK(d->BindImageMemory(dv, c->app_images[i], c->app_memory[i], 0));
	}

	VkAttachmentDescription att = {
		.format = c->format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};
	VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1, .pColorAttachments = &ref };
	VkSubpassDependency dep = {
		.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub,
		.dependencyCount = 1, .pDependencies = &dep };
	CHECK(d->CreateRenderPass(dv, &rpi, NULL, &c->render_pass));

	VkImageViewCreateInfo vci = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = c->format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	for (uint32_t i = 0; i < n; i++) {
		vci.image = c->app_images[i];
		CHECK(d->CreateImageView(dv, &vci, NULL, &c->app_views[i]));
		vci.image = c->real_images[i];
		CHECK(d->CreateImageView(dv, &vci, NULL, &c->real_views[i]));
		VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = c->render_pass, .attachmentCount = 1, .pAttachments = &c->real_views[i],
			.width = c->real.width, .height = c->real.height, .layers = 1 };
		CHECK(d->CreateFramebuffer(dv, &fci, NULL, &c->framebuffers[i]));
	}

	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo sli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1, .pBindings = &b };
	CHECK(d->CreateDescriptorSetLayout(dv, &sli, NULL, &c->set_layout));
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n };
	VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = n, .poolSizeCount = 1, .pPoolSizes = &ps };
	CHECK(d->CreateDescriptorPool(dv, &pci, NULL, &c->pool));
	for (uint32_t i = 0; i < n; i++) {
		VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = c->pool, .descriptorSetCount = 1, .pSetLayouts = &c->set_layout };
		CHECK(d->AllocateDescriptorSets(dv, &dai, &c->sets[i]));
		VkDescriptorImageInfo dii = { VK_NULL_HANDLE, c->app_views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->sets[i],
			.dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &dii };
		d->UpdateDescriptorSets(dv, 1, &w, 0, NULL);
	}

	VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &c->set_layout, .pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr };
	CHECK(d->CreatePipelineLayout(dv, &pli, NULL, &c->pipeline_layout));

	VkShaderModule vs, fs;
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(rotate_vert_spv), .pCode = rotate_vert_spv };
	CHECK(d->CreateShaderModule(dv, &smi, NULL, &vs));
	smi.codeSize = sizeof(rotate_frag_spv);
	smi.pCode = rotate_frag_spv;
	VkResult r = d->CreateShaderModule(dv, &smi, NULL, &fs);
	if (r != VK_SUCCESS) {
		d->DestroyShaderModule(dv, vs, NULL);
		say("fragment shader: %d", r);
		return r;
	}
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
		  .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		  .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, (float)c->real.width, (float)c->real.height, 0, 1 };
	VkRect2D sc = { { 0, 0 }, c->real };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
	VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
		.pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
		.pColorBlendState = &cb, .layout = c->pipeline_layout, .renderPass = c->render_pass };
	r = d->CreateGraphicsPipelines(dv, VK_NULL_HANDLE, 1, &gpi, NULL, &c->pipeline);
	d->DestroyShaderModule(dv, vs, NULL);
	d->DestroyShaderModule(dv, fs, NULL);
	if (r != VK_SUCCESS) {
		say("pipeline: %d", r);
		return r;
	}

	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	for (uint32_t i = 0; i < n; i++) {
		CHECK(d->CreateFence(dv, &fci, NULL, &c->fences[i]));
		CHECK(d->CreateSemaphore(dv, &sci, NULL, &c->done[i]));
	}
	if (stats_on && d->timestamp_period > 0) {
		VkQueryPoolCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
			.queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * n };
		if (d->CreateQueryPool(dv, &qci, NULL, &c->queries) != VK_SUCCESS)
			c->queries = VK_NULL_HANDLE;
	}
	return VK_SUCCESS;
}

/* The command buffers: one per image, recorded once (image i of the app's into image i of the
 * panel's), for the family of the queue that presents. */
static VkResult chain_record(chain *c, uint32_t family)
{
	dev *d = c->d;
	VkDevice dv = d->device;
	VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = family };
	CHECK(d->CreateCommandPool(dv, &cpi, NULL, &c->cmd_pool));
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = c->cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = c->count };
	CHECK(d->AllocateCommandBuffers(dv, &cai, c->cmds));

	/* Panel pixel (x, y) shows app pixel (k + a*x + b*y, k' + a'*x + b'*y). Turned clockwise, the
	 * app's top-left goes to the panel's top-right; counter-clockwise, to its bottom-left. */
	int32_t pc[8];
	if (turn_cw) {
		int32_t m[8] = { 0, 0, 1, 0, (int32_t)c->app.height - 1, -1, 0, 0 };
		memcpy(pc, m, sizeof(pc));
	} else {
		int32_t m[8] = { (int32_t)c->app.width - 1, 0, -1, 0, 0, 1, 0, 0 };
		memcpy(pc, m, sizeof(pc));
	}

	for (uint32_t i = 0; i < c->count; i++) {
		VkCommandBuffer cb = c->cmds[i];
		if (d->set_loader_data)
			d->set_loader_data(dv, cb);
		VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		CHECK(d->BeginCommandBuffer(cb, &bi));
		if (c->queries) {
			d->CmdResetQueryPool(cb, c->queries, 2 * i, 2);
			d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->queries, 2 * i);
		}
		VkImageMemoryBarrier to_read = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = c->app_images[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		                      0, 0, NULL, 0, NULL, 1, &to_read);
		VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = c->render_pass, .framebuffer = c->framebuffers[i],
			.renderArea = { { 0, 0 }, c->real } };
		d->CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipeline);
		d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, c->pipeline_layout, 0, 1, &c->sets[i], 0, NULL);
		d->CmdPushConstants(cb, c->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
		d->CmdDraw(cb, 3, 1, 0, 0);
		d->CmdEndRenderPass(cb);
		/* Back to the layout the app gave it, as the app expects of an image it acquires again. */
		VkImageMemoryBarrier back = to_read;
		back.srcAccessMask = 0;
		back.dstAccessMask = 0;
		back.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                      0, 0, NULL, 0, NULL, 1, &back);
		if (c->queries)
			d->CmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->queries, 2 * i + 1);
		CHECK(d->EndCommandBuffer(cb));
	}
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
	dev *d = dev_of(device);
	VkExtent2D app;
	if (!surface_turned(info->surface, &app) || info->imageExtent.width <= info->imageExtent.height)
		return d->CreateSwapchainKHR(device, info, alloc, out);

	chain *c = calloc(1, sizeof(*c));
	if (!c)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	c->d = d;
	c->app = info->imageExtent;
	c->real = (VkExtent2D){ info->imageExtent.height, info->imageExtent.width };
	c->format = info->imageFormat;

	VkSwapchainCreateInfoKHR ci = *info;
	ci.imageExtent = c->real;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.queueFamilyIndexCount = 0;
	ci.pQueueFamilyIndices = NULL;
	VkResult r = d->CreateSwapchainKHR(device, &ci, alloc, &c->swapchain);
	if (r != VK_SUCCESS) {
		free(c);
		return r;
	}
	uint32_t n = 0;
	d->GetSwapchainImagesKHR(device, c->swapchain, &n, NULL);
	c->count = n;
	c->real_images = calloc(n ? n : 1, sizeof(VkImage));
	if (c->real_images)
		d->GetSwapchainImagesKHR(device, c->swapchain, &n, c->real_images);
	r = c->real_images ? chain_build(c, info) : VK_ERROR_OUT_OF_HOST_MEMORY;
	if (r != VK_SUCCESS) {
		VkSwapchainKHR s = c->swapchain;
		chain_free(c);
		d->DestroySwapchainKHR(device, s, alloc);
		return r;
	}
	pthread_mutex_lock(&lock);
	c->next = chains;
	chains = c;
	pthread_mutex_unlock(&lock);
	*out = c->swapchain;
	say("swapchain: %u images, the app's %ux%u, the panel's %ux%u", n, c->app.width, c->app.height,
	    c->real.width, c->real.height);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR s,
		const VkAllocationCallbacks *alloc)
{
	dev *d = dev_of(device);
	chain *dead = NULL;
	pthread_mutex_lock(&lock);
	for (chain **p = &chains; *p; p = &(*p)->next)
		if ((*p)->swapchain == s) {
			dead = *p;
			*p = dead->next;
			break;
		}
	pthread_mutex_unlock(&lock);
	if (dead)
		chain_free(dead);
	d->DestroySwapchainKHR(device, s, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR s,
		uint32_t *count, VkImage *images)
{
	dev *d = dev_of(device);
	chain *c = chain_of(s);
	if (!c)
		return d->GetSwapchainImagesKHR(device, s, count, images);
	if (!images) {
		*count = c->count;
		return VK_SUCCESS;
	}
	uint32_t n = *count < c->count ? *count : c->count;
	memcpy(images, c->app_images, n * sizeof(VkImage));
	*count = n;
	return n < c->count ? VK_INCOMPLETE : VK_SUCCESS;
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	dev *d = dev_of(queue);
	uint32_t n = info->swapchainCount, turned = 0;
	chain *cs[8] = { 0 };
	for (uint32_t i = 0; i < n && i < 8; i++)
		if ((cs[i] = chain_of(info->pSwapchains[i])))
			turned++;
	if (!turned || n > 8)
		return d->QueuePresentKHR(queue, info);

	double t0 = stats_on ? now_ms() : 0;
	VkCommandBuffer cmds[8];
	VkSemaphore signal[8];
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		chain *c = cs[i];
		if (!c)
			continue;
		uint32_t idx = info->pImageIndices[i];
		if (!c->cmd_pool) {
			VkResult r = chain_record(c, queue_family(d, queue));
			if (r != VK_SUCCESS)
				return r;
		}
		if (c->submitted[idx]) {
			d->WaitForFences(d->device, 1, &c->fences[idx], VK_TRUE, UINT64_MAX);
			if (c->queries) {
				uint64_t ts[2];
				if (d->GetQueryPoolResults(d->device, c->queries, 2 * idx, 2, sizeof(ts), ts, sizeof(uint64_t),
				                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
					c->gpu_ms += (double)(ts[1] - ts[0]) * d->timestamp_period / 1e6, c->gpu_samples++;
			}
		}
		d->ResetFences(d->device, 1, &c->fences[idx]);
		c->submitted[idx] = true;
		cmds[k] = c->cmds[idx];
		signal[k] = c->done[idx];
		k++;
	}
	/* One submit per turned swapchain, each with its own fence; the first also waits on the app. */
	VkPipelineStageFlags stages[16];
	for (uint32_t i = 0; i < 16; i++)
		stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	uint32_t waits = info->waitSemaphoreCount;
	const VkSemaphore *wait = info->pWaitSemaphores;
	VkPipelineStageFlags *wait_stages = stages;
	VkPipelineStageFlags *heap_stages = NULL;
	if (waits > 16) {
		heap_stages = malloc(waits * sizeof(*heap_stages));
		if (!heap_stages)
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		for (uint32_t i = 0; i < waits; i++)
			heap_stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		wait_stages = heap_stages;
	}
	VkResult r = VK_SUCCESS;
	uint32_t j = 0;
	for (uint32_t i = 0; i < n && r == VK_SUCCESS; i++) {
		chain *c = cs[i];
		if (!c)
			continue;
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.waitSemaphoreCount = j == 0 ? waits : 0, .pWaitSemaphores = wait, .pWaitDstStageMask = wait_stages,
			.commandBufferCount = 1, .pCommandBuffers = &cmds[j], .signalSemaphoreCount = 1,
			.pSignalSemaphores = &signal[j] };
		r = d->QueueSubmit(queue, 1, &si, c->fences[info->pImageIndices[i]]);
		j++;
	}
	free(heap_stages);
	if (r != VK_SUCCESS) {
		say("submit: %d", r);
		return r;
	}

	/* The present waits on the turns (which waited on the app's semaphores). Swapchains of the
	 * same present that aren't turned are covered too: the turn's submit waited on everything. */
	VkPresentInfoKHR pi = *info;
	pi.waitSemaphoreCount = k;
	pi.pWaitSemaphores = signal;
	r = d->QueuePresentKHR(queue, &pi);

	if (stats_on) {
		double t1 = now_ms();
		for (uint32_t i = 0; i < n; i++) {
			chain *c = cs[i];
			if (!c)
				continue;
			c->cpu_ms += t1 - t0;
			if (++c->frames % STATS_EVERY == 0) {
				fprintf(stderr, "[basedos-vk-rotate] %ux%u -> %ux%u: GPU %.3f ms, present %.3f ms a frame (%u frames)\n",
				        c->app.width, c->app.height, c->real.width, c->real.height,
				        c->gpu_samples ? c->gpu_ms / c->gpu_samples : 0.0, c->cpu_ms / STATS_EVERY, STATS_EVERY);
				c->gpu_ms = c->cpu_ms = 0;
				c->gpu_samples = 0;
			}
		}
	}
	return r;
}

/* ---- Instance and device creation, and the loader's entry points ----------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *info,
		const VkAllocationCallbacks *alloc, VkInstance *out)
{
	settings();
	VkLayerInstanceCreateInfo *link = (VkLayerInstanceCreateInfo *)info->pNext;
	while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
		link = (VkLayerInstanceCreateInfo *)link->pNext;
	if (!link)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkResult r = create(info, alloc, out);
	if (r != VK_SUCCESS)
		return r;
	inst *in = calloc(1, sizeof(*in));
	if (!in)
		return VK_SUCCESS; /* the layer just stays out of it */
	in->key = key_of(*out);
	in->instance = *out;
	in->gipa = gipa;
#define I(name) in->name = (PFN_vk##name)gipa(*out, "vk" #name);
	I(DestroyInstance) I(EnumeratePhysicalDevices) I(GetPhysicalDeviceProperties)
	I(GetPhysicalDeviceMemoryProperties) I(EnumerateDeviceExtensionProperties)
	I(GetPhysicalDeviceDisplayPropertiesKHR) I(GetPhysicalDeviceDisplayProperties2KHR)
	I(GetDisplayModePropertiesKHR) I(GetDisplayModeProperties2KHR) I(CreateDisplayModeKHR)
	I(GetDisplayPlaneCapabilitiesKHR) I(GetDisplayPlaneCapabilities2KHR) I(CreateDisplayPlaneSurfaceKHR)
	I(DestroySurfaceKHR) I(GetPhysicalDeviceSurfaceCapabilitiesKHR) I(GetPhysicalDeviceSurfaceCapabilities2KHR)
#undef I
	pthread_mutex_lock(&lock);
	in->next = instances;
	instances = in;
	pthread_mutex_unlock(&lock);
	say("instance created; frames turned %s", turn_cw ? "cw" : "ccw");
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *alloc)
{
	inst *in = inst_of(instance);
	PFN_vkDestroyInstance destroy = in->DestroyInstance;
	pthread_mutex_lock(&lock);
	for (inst **p = &instances; *p; p = &(*p)->next)
		if (*p == in) {
			*p = in->next;
			break;
		}
	for (phys **p = &physicals; *p;)
		if ((*p)->in == in) {
			phys *dead = *p;
			*p = dead->next;
			free(dead);
		} else {
			p = &(*p)->next;
		}
	pthread_mutex_unlock(&lock);
	free(in);
	destroy(instance, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *count,
		VkPhysicalDevice *pds)
{
	inst *in = inst_of(instance);
	VkResult r = in->EnumeratePhysicalDevices(instance, count, pds);
	if (pds && r >= 0) {
		pthread_mutex_lock(&lock);
		for (uint32_t i = 0; i < *count; i++) {
			bool known = false;
			for (phys *p = physicals; p; p = p->next)
				known |= p->pd == pds[i];
			if (known)
				continue;
			phys *p = calloc(1, sizeof(*p));
			if (p) {
				p->pd = pds[i];
				p->in = in;
				p->next = physicals;
				physicals = p;
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice pd,
		const char *layer, uint32_t *count, VkExtensionProperties *props)
{
	if (layer && strcmp(layer, LAYER_NAME) == 0) {
		*count = 0;
		return VK_SUCCESS;
	}
	inst *in = inst_of_pd(pd);
	return in->EnumerateDeviceExtensionProperties(pd, layer, count, props);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *alloc, VkDevice *out)
{
	VkLayerDeviceCreateInfo *link = (VkLayerDeviceCreateInfo *)info->pNext;
	while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
		link = (VkLayerDeviceCreateInfo *)link->pNext;
	VkLayerDeviceCreateInfo *data = (VkLayerDeviceCreateInfo *)info->pNext;
	while (data && !(data->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && data->function == VK_LOADER_DATA_CALLBACK))
		data = (VkLayerDeviceCreateInfo *)data->pNext;
	if (!link)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	inst *in = inst_of_pd(pd);
	PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(in ? in->instance : VK_NULL_HANDLE, "vkCreateDevice");
	VkResult r = create(pd, info, alloc, out);
	if (r != VK_SUCCESS)
		return r;
	dev *d = calloc(1, sizeof(*d));
	if (!d)
		return VK_SUCCESS;
	d->key = key_of(*out);
	d->device = *out;
	d->pd = pd;
	d->in = in;
	d->set_loader_data = data ? data->u.pfnSetDeviceLoaderData : NULL;
	d->GetDeviceProcAddr = gdpa;
#define D(name) d->name = (PFN_vk##name)gdpa(*out, "vk" #name);
	D(DestroyDevice) D(GetDeviceQueue) D(GetDeviceQueue2) D(DeviceWaitIdle)
	D(CreateSwapchainKHR) D(DestroySwapchainKHR) D(GetSwapchainImagesKHR) D(AcquireNextImageKHR)
	D(AcquireNextImage2KHR) D(QueuePresentKHR) D(QueueSubmit)
	D(CreateImage) D(DestroyImage) D(GetImageMemoryRequirements) D(AllocateMemory) D(FreeMemory)
	D(BindImageMemory) D(CreateImageView) D(DestroyImageView) D(CreateRenderPass) D(DestroyRenderPass)
	D(CreateFramebuffer) D(DestroyFramebuffer) D(CreateShaderModule) D(DestroyShaderModule)
	D(CreateDescriptorSetLayout) D(DestroyDescriptorSetLayout) D(CreateDescriptorPool)
	D(DestroyDescriptorPool) D(AllocateDescriptorSets) D(UpdateDescriptorSets) D(CreatePipelineLayout)
	D(DestroyPipelineLayout) D(CreateGraphicsPipelines) D(DestroyPipeline) D(CreateCommandPool)
	D(DestroyCommandPool) D(AllocateCommandBuffers) D(BeginCommandBuffer) D(EndCommandBuffer)
	D(CmdPipelineBarrier) D(CmdBeginRenderPass) D(CmdEndRenderPass) D(CmdBindPipeline)
	D(CmdBindDescriptorSets) D(CmdPushConstants) D(CmdDraw) D(CmdResetQueryPool) D(CmdWriteTimestamp)
	D(CreateFence) D(DestroyFence) D(WaitForFences) D(ResetFences) D(CreateSemaphore) D(DestroySemaphore)
	D(CreateQueryPool) D(DestroyQueryPool) D(GetQueryPoolResults)
#undef D
	if (in) {
		in->GetPhysicalDeviceMemoryProperties(pd, &d->mem);
		VkPhysicalDeviceProperties props;
		in->GetPhysicalDeviceProperties(pd, &props);
		d->timestamp_period = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0;
	}
	pthread_mutex_lock(&lock);
	d->next = devices;
	devices = d;
	pthread_mutex_unlock(&lock);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *alloc)
{
	dev *d = dev_of(device);
	PFN_vkDestroyDevice destroy = d->DestroyDevice;
	pthread_mutex_lock(&lock);
	chain *mine = NULL;
	for (chain **p = &chains; *p;)
		if ((*p)->d == d) {
			chain *c = *p;
			*p = c->next;
			c->next = mine;
			mine = c;
		} else {
			p = &(*p)->next;
		}
	for (dev **p = &devices; *p; p = &(*p)->next)
		if (*p == d) {
			*p = d->next;
			break;
		}
	pthread_mutex_unlock(&lock);
	while (mine) {
		chain *c = mine;
		mine = c->next;
		chain_free(c);
	}
	free(d);
	destroy(device, alloc);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name);

static PFN_vkVoidFunction intercept_device(const char *name)
{
#define F(n) if (strcmp(name, "vk" #n) == 0) return (PFN_vkVoidFunction)n;
	F(GetDeviceProcAddr) F(DestroyDevice) F(GetDeviceQueue) F(GetDeviceQueue2) F(CreateSwapchainKHR)
	F(DestroySwapchainKHR) F(GetSwapchainImagesKHR) F(QueuePresentKHR)
#undef F
	return NULL;
}

static PFN_vkVoidFunction intercept(const char *name)
{
#define F(n) if (strcmp(name, "vk" #n) == 0) return (PFN_vkVoidFunction)n;
	F(CreateInstance) F(DestroyInstance) F(EnumeratePhysicalDevices) F(EnumerateDeviceExtensionProperties)
	F(CreateDevice) F(GetPhysicalDeviceDisplayPropertiesKHR) F(GetPhysicalDeviceDisplayProperties2KHR)
	F(GetDisplayModePropertiesKHR) F(GetDisplayModeProperties2KHR) F(CreateDisplayModeKHR)
	F(GetDisplayPlaneCapabilitiesKHR) F(GetDisplayPlaneCapabilities2KHR) F(CreateDisplayPlaneSurfaceKHR)
	F(DestroySurfaceKHR) F(GetPhysicalDeviceSurfaceCapabilitiesKHR) F(GetPhysicalDeviceSurfaceCapabilities2KHR)
#undef F
	return intercept_device(name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char *name)
{
	dev *d = dev_of(device);
	if (!d)
		return NULL;
	PFN_vkVoidFunction next = d->GetDeviceProcAddr(device, name);
	PFN_vkVoidFunction mine = intercept_device(name);
	/* Only what the driver has: an app asking for a missing function must still get NULL. */
	return mine && next ? mine : next;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *name)
{
	if (strcmp(name, "vkGetInstanceProcAddr") == 0)
		return (PFN_vkVoidFunction)GetInstanceProcAddr;
	if (strcmp(name, "vkCreateInstance") == 0)
		return (PFN_vkVoidFunction)CreateInstance;
	if (!instance)
		return NULL;
	inst *in = inst_of(instance);
	if (!in)
		return NULL;
	PFN_vkVoidFunction next = in->gipa(instance, name);
	PFN_vkVoidFunction mine = intercept(name);
	return mine && next ? mine : next;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v)
{
	if (v->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;
	v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
