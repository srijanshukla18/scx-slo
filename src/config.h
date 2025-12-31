/* SPDX-License-Identifier: GPL-2.0 */
/*
 * SLO configuration interface header
 */
#ifndef __SCX_SLO_CONFIG_H
#define __SCX_SLO_CONFIG_H

#include "scx_slo.h"

/* Load SLO configuration from file and update BPF maps */
int load_slo_config(int slo_map_fd);

/* Create example configuration file */
int create_example_config(void);

#endif /* __SCX_SLO_CONFIG_H */