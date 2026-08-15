#ifndef __GENL_VERSION_H__
#define __GENL_VERSION_H__

#include "genl.h"

#define DRV_VERSION "0.10.2"

enum gtp5g_version {
    GTP5G_VERSION,
    GTP5G_SHARED_MARK_ABI
};

int gtp5g_genl_get_version(struct sk_buff *, struct genl_info *);

#endif // __GENL_VERSION_H__
