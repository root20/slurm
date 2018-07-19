/*****************************************************************************\
 *  acct_gather_energy_xcc_config.c - functions for reading acct_gather.conf
 *****************************************************************************
 *  Copyright (C) 2018
 *  Written by SchedMD - Felip Moll
 *  Based on IPMI plugin by Thomas Cadeau @ Bull
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "acct_gather_energy_xcc_config.h"

extern void reset_slurm_ipmi_conf(slurm_ipmi_conf_t *slurm_ipmi_conf)
{
	if (slurm_ipmi_conf) {
		slurm_ipmi_conf->adjustment = false;
		slurm_ipmi_conf->authentication_type = -1;
		slurm_ipmi_conf->cipher_suite_id = 0;
		xfree(slurm_ipmi_conf->driver_device);
		slurm_ipmi_conf->driver_type = -1;
		slurm_ipmi_conf->disable_auto_probe = 0;
		slurm_ipmi_conf->driver_address = 0;
		slurm_ipmi_conf->freq = DEFAULT_IPMI_FREQ;
		xfree(slurm_ipmi_conf->k_g);
		slurm_ipmi_conf->k_g_len = 0;
		xfree(slurm_ipmi_conf->password);
		slurm_ipmi_conf->password = xstrdup(DEFAULT_IPMI_PASS);
		slurm_ipmi_conf->privilege_level = -1;
		slurm_ipmi_conf->protocol_version = -1;
		slurm_ipmi_conf->register_spacing = 0;
		slurm_ipmi_conf->retransmission_timeout = 0;
		slurm_ipmi_conf->session_timeout = 0;
		slurm_ipmi_conf->timeout = DEFAULT_IPMI_TIMEOUT;
		xfree(slurm_ipmi_conf->username);
		slurm_ipmi_conf->username = xstrdup(DEFAULT_IPMI_USER);
		slurm_ipmi_conf->workaround_flags = 0; // See man 8 ipmi-raw
		slurm_ipmi_conf->ipmi_flags = IPMI_FLAGS_DEFAULT; //IPMI_FLAGS_DEBUG_DUMP
	}
}
