/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

static int vgdisplay_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg, int consistent,
			    void *handle)
{
	/* FIXME Do the active check here if activevolumegroups_ARG ? */
	if (!vg) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
		return ECMD_FAILED;
	}

	if (!consistent)
		log_error("WARNING: Volume group \"%s\" inconsistent", vg_name);

	if (vg->status & EXPORTED_VG)
		log_print("WARNING: volume group \"%s\" is exported", vg_name);

	if (arg_count(cmd, colon_ARG)) {
		vgdisplay_colons(vg);
		return 0;
	}

	if (arg_count(cmd, short_ARG)) {
		vgdisplay_short(vg);
		return 0;
	}

	vgdisplay_full(vg);	/* was vg_show */

	if (arg_count(cmd, verbose_ARG)) {
		vgdisplay_extents(vg);

		process_each_lv_in_vg(cmd, vg, NULL, &lvdisplay_full);

		log_print("--- Physical volumes ---");
		process_each_pv_in_vg(cmd, vg, NULL, &pvdisplay_short);
	}

	return 0;
}

int vgdisplay(struct cmd_context *cmd, int argc, char **argv)
{
	if (arg_count(cmd, columns_ARG)) {
		if (arg_count(cmd, colon_ARG) ||
		    arg_count(cmd, activevolumegroups_ARG) ||
		    arg_count(cmd, short_ARG)) {
			log_error("Incompatible options selected");
			return EINVALID_CMD_LINE;
		}
		return vgs(cmd, argc, argv);
	} else if (arg_count(cmd, aligned_ARG) ||
		   arg_count(cmd, noheadings_ARG) ||
		   arg_count(cmd, options_ARG) ||
		   arg_count(cmd, separator_ARG) ||
		   arg_count(cmd, sort_ARG) || arg_count(cmd, unbuffered_ARG)) {
		log_error("Incompatible options selected");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, colon_ARG) && arg_count(cmd, short_ARG)) {
		log_error("Option -c is not allowed with option -s");
		return EINVALID_CMD_LINE;
	}

	if (argc && arg_count(cmd, activevolumegroups_ARG)) {
		log_error("Option -A is not allowed with volume group names");
		return EINVALID_CMD_LINE;
	}

/********* FIXME: Do without this - or else 2(+) passes! 
	   Figure out longest volume group name 
	for (c = opt; opt < argc; opt++) {
		len = strlen(argv[opt]);
		if (len > max_len)
			max_len = len;
	}
**********/

	process_each_vg(cmd, argc, argv, LCK_VG_READ, 0, NULL,
			&vgdisplay_single);

/******** FIXME Need to count number processed 
	  Add this to process_each_vg if arg_count(cmd,activevolumegroups_ARG) ? 

	if (opt == argc) {
		log_print("no ");
		if (arg_count(cmd,activevolumegroups_ARG))
			printf("active ");
		printf("volume groups found\n\n");
		return LVM_E_NO_VG;
	}
************/

	return 0;
}
