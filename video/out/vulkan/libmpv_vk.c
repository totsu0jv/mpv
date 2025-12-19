/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <libplacebo/vulkan.h>

#include "common.h"
#include "context.h"
#include "mpv/render_vk.h"
#include "options/m_config.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"

struct priv {
    pl_log pllog;
    pl_vulkan vulkan;
    pl_gpu gpu;
    struct ra_ctx *ra_ctx;
    struct ra_tex proxy_tex;
};

static int init(struct libmpv_gpu_context *ctx, mpv_render_param *params)
{
    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *p = ctx->priv;

    mpv_vulkan_init_params *vk_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, NULL);
    if (!vk_params)
        return MPV_ERROR_INVALID_PARAMETER;

    if (!vk_params->instance || !vk_params->physical_device ||
        !vk_params->device || !vk_params->graphics_queue) {
        MP_ERR(ctx, "Missing required Vulkan handles\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Create libplacebo log
    p->pllog = mppl_log_create(ctx, ctx->log);
    if (!p->pllog) {
        MP_ERR(ctx, "Failed to create libplacebo log\n");
        return MPV_ERROR_GENERIC;
    }

    // Import user's Vulkan device into libplacebo
    PFN_vkGetInstanceProcAddr get_proc = vk_params->get_instance_proc_addr;
    if (!get_proc)
        get_proc = vkGetInstanceProcAddr;

    struct pl_vulkan_import_params import_params = {
        .instance = vk_params->instance,
        .phys_device = vk_params->physical_device,
        .device = vk_params->device,
        .get_proc_addr = get_proc,
        .queue_graphics = {
            .index = vk_params->graphics_queue_family,
            .count = 1,
        },
        .features = vk_params->features,
    };

    p->vulkan = pl_vulkan_import(p->pllog, &import_params);
    if (!p->vulkan) {
        MP_ERR(ctx, "Failed to import Vulkan device\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    p->gpu = p->vulkan->gpu;

    // Create a minimal ra_ctx for the renderer
    p->ra_ctx = talloc_zero(p, struct ra_ctx);
    p->ra_ctx->log = ctx->log;
    p->ra_ctx->global = ctx->global;
    p->ra_ctx->opts = (struct ra_ctx_opts) {
        .allow_sw = true,
    };

    // Create ra from libplacebo gpu
    p->ra_ctx->ra = ra_create_pl(p->gpu, ctx->log);
    if (!p->ra_ctx->ra) {
        MP_ERR(ctx, "Failed to create ra from pl_gpu\n");
        return MPV_ERROR_UNSUPPORTED;
    }

    ctx->ra_ctx = p->ra_ctx;
    return 0;
}

static int wrap_fbo(struct libmpv_gpu_context *ctx, mpv_render_param *params,
                    struct ra_tex **out)
{
    struct priv *p = ctx->priv;

    mpv_vulkan_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_VULKAN_FBO, NULL);
    if (!fbo)
        return MPV_ERROR_INVALID_PARAMETER;

    if (!fbo->image || !fbo->width || !fbo->height) {
        MP_ERR(ctx, "Invalid Vulkan FBO parameters\n");
        return MPV_ERROR_INVALID_PARAMETER;
    }

    // Wrap the VkImage as pl_tex using pl_vulkan_wrap
    struct pl_vulkan_wrap_params wrap_params = {
        .image = fbo->image,
        .width = fbo->width,
        .height = fbo->height,
        .format = fbo->format,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    pl_tex pltex = pl_vulkan_wrap(p->gpu, &wrap_params);
    if (!pltex) {
        MP_ERR(ctx, "Failed to wrap VkImage as pl_tex\n");
        return MPV_ERROR_GENERIC;
    }

    // Release the texture to libplacebo - it starts out "held" by user
    // We need to tell libplacebo it can now use the texture
    struct pl_vulkan_release_params release_params = {
        .tex = pltex,
        .layout = fbo->current_layout,
        .qf = VK_QUEUE_FAMILY_IGNORED,
    };
    pl_vulkan_release_ex(p->gpu, &release_params);

    // Wrap pl_tex as ra_tex
    if (!mppl_wrap_tex(p->ra_ctx->ra, pltex, &p->proxy_tex)) {
        pl_tex_destroy(p->gpu, &pltex);
        return MPV_ERROR_GENERIC;
    }

    *out = &p->proxy_tex;
    return 0;
}

static void done_frame(struct libmpv_gpu_context *ctx, bool ds)
{
    struct priv *p = ctx->priv;

    // Ensure GPU work is complete
    pl_gpu_finish(p->gpu);
}

static void destroy(struct libmpv_gpu_context *ctx)
{
    struct priv *p = ctx->priv;
    if (!p)
        return;

    if (p->ra_ctx && p->ra_ctx->ra) {
        p->ra_ctx->ra->fns->destroy(p->ra_ctx->ra);
        p->ra_ctx->ra = NULL;
    }

    if (p->vulkan) {
        pl_vulkan_destroy(&p->vulkan);
    }

    pl_log_destroy(&p->pllog);
}

const struct libmpv_gpu_context_fns libmpv_gpu_context_vk = {
    .api_name = MPV_RENDER_API_TYPE_VULKAN,
    .init = init,
    .wrap_fbo = wrap_fbo,
    .done_frame = done_frame,
    .destroy = destroy,
};
