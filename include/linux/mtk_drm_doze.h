/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MTK_DRM_DOZE_H
#define _LINUX_MTK_DRM_DOZE_H

#include <linux/kconfig.h>
#include <linux/notifier.h>
#include <linux/types.h>

enum mtk_drm_doze_event {
	MTK_DRM_DOZE_ENTER = 1,
	MTK_DRM_DOZE_EXIT,
};

struct mtk_drm_doze_state {
	unsigned int active;
	unsigned int crtc_index;
};

#if IS_REACHABLE(CONFIG_DRM_MEDIATEK)
int mtk_drm_doze_register_client(struct notifier_block *nb);
int mtk_drm_doze_unregister_client(struct notifier_block *nb);
#else
static inline int mtk_drm_doze_register_client(struct notifier_block *nb)
{
	return 0;
}

static inline int mtk_drm_doze_unregister_client(struct notifier_block *nb)
{
	return 0;
}
#endif

#endif /* _LINUX_MTK_DRM_DOZE_H */
