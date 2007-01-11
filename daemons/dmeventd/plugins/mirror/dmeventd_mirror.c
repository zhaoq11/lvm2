/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "libdevmapper.h"
#include "libdevmapper-event.h"
#include "lvm2cmd.h"
#include "lvm-string.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include <syslog.h> /* FIXME Replace syslog with multilog */
/* FIXME Missing openlog? */

#define ME_IGNORE    0
#define ME_INSYNC    1
#define ME_FAILURE   2

/*
 * register_device() is called first and performs initialisation.
 * Only one device may be registered or unregistered at a time.
 */
static pthread_mutex_t _register_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Number of active registrations.
 */
static int _register_count = 0;

static struct dm_pool *_mem_pool = NULL;
static void *_lvm_handle = NULL;

/*
 * Currently only one event can be processed at a time.
 */
static pthread_mutex_t _event_mutex = PTHREAD_MUTEX_INITIALIZER;

static int _get_mirror_event(char *params)
{
	int i, r = ME_INSYNC;
	char **args = NULL;
	char *dev_status_str;
	char *log_status_str;
	char *sync_str;
	char *p = NULL;
	int log_argc, num_devs;

	/*
	 * Unused:  0 409600 mirror
	 * Used  :  2 253:4 253:5 400/400 1 AA 3 cluster 253:3 A
	 */

	/* number of devices */
	if (!dm_split_words(params, 1, 0, &p))
		goto out_parse;

	if (!(num_devs = atoi(p)))
		goto out_parse;
	p += strlen(p) + 1;

	/* devices names + max log parameters */
	args = dm_malloc((num_devs + 8) * sizeof(char *));
	if (!args || dm_split_words(p, num_devs + 8, 0, args) < num_devs + 8)
		goto out_parse;

	dev_status_str = args[2 + num_devs];
	log_argc = atoi(args[3 + num_devs]);
	log_status_str = args[3 + num_devs + log_argc];
	sync_str = args[num_devs];

	/* Check for bad mirror devices */
	for (i = 0; i < num_devs; i++)
		if (dev_status_str[i] == 'D') {
			syslog(LOG_ERR, "Mirror device, %s, has failed.\n", args[i]);
			r = ME_FAILURE;
		}

	/* Check for bad disk log device */
	if (log_argc > 1 && log_status_str[0] == 'D') {
		syslog(LOG_ERR, "Log device, %s, has failed.\n",
		       args[2 + num_devs + log_argc]);
		r = ME_FAILURE;
	}

	if (r == ME_FAILURE)
		goto out;

	p = strstr(sync_str, "/");
	if (p) {
		p[0] = '\0';
		if (strcmp(sync_str, p+1))
			r = ME_IGNORE;
		p[0] = '/';
	} else
		goto out_parse;

out:
	if (args)
		dm_free(args);
	return r;
	
out_parse:
	if (args)
		dm_free(args);
	syslog(LOG_ERR, "Unable to parse mirror status string.");
	return ME_IGNORE;
}

static void _temporary_log_fn(int level, const char *file,
			      int line, const char *format)
{
	if (!strncmp(format, "WARNING: ", 9) && (level < 5))
		syslog(LOG_CRIT, "%s", format);
	else
		syslog(LOG_DEBUG, "%s", format);
}

static int _remove_failed_devices(const char *device)
{
	int r;
#define CMD_SIZE 256	/* FIXME Use system restriction */
	char cmd_str[CMD_SIZE];
	char *vg = NULL, *lv = NULL, *layer = NULL;

	if (strlen(device) > 200)  /* FIXME Use real restriction */
		return -ENAMETOOLONG;	/* FIXME These return code distinctions are not used so remove them! */

	if (!dm_split_lvm_name(_mem_pool, device, &vg, &lv, &layer)) {
		syslog(LOG_ERR, "Unable to determine VG name from %s",
		       device);
		return -ENOMEM;	/* FIXME Replace with generic error return - reason for failure has already got logged */
	}

	/* FIXME Is any sanity-checking required on %s? */
	if (CMD_SIZE <= snprintf(cmd_str, CMD_SIZE, "vgreduce --removemissing %s", vg)) {
		/* this error should be caught above, but doesn't hurt to check again */
		syslog(LOG_ERR, "Unable to form LVM command: Device name too long");
		dm_pool_empty(_mem_pool);  /* FIXME: not safe with multiple threads */
		return -ENAMETOOLONG; /* FIXME Replace with generic error return - reason for failure has already got logged */
	}

	r = lvm2_run(_lvm_handle, cmd_str);

	dm_pool_empty(_mem_pool);  /* FIXME: not safe with multiple threads */
	return (r == 1) ? 0 : -1;
}

void process_event(const char *device, enum dm_event_type event)
{
	struct dm_task *dmt;
	void *next = NULL;
	uint64_t start, length;
	char *target_type = NULL;
	char *params;

	if (pthread_mutex_trylock(&_event_mutex)) {
		syslog(LOG_NOTICE, "Another thread is handling an event.  Waiting...");
		pthread_mutex_lock(&_event_mutex);
	}
	/* FIXME Move inside libdevmapper */
	if (!(dmt = dm_task_create(DM_DEVICE_STATUS))) {
		syslog(LOG_ERR, "Unable to create dm_task.\n");
		goto fail;
	}

	if (!dm_task_set_name(dmt, device)) {
		syslog(LOG_ERR, "Unable to set device name.\n");
		goto fail;
	}

	if (!dm_task_run(dmt)) {
		syslog(LOG_ERR, "Unable to run task.\n");
		goto fail;
	}

	do {
		next = dm_get_next_target(dmt, next, &start, &length,
					  &target_type, &params);

		if (!target_type) {
			syslog(LOG_INFO, "%s mapping lost.\n", device);
			continue;
		}

		if (strcmp(target_type, "mirror")) {
			syslog(LOG_INFO, "%s has unmirrored portion.\n", device);
			continue;
		}

		switch(_get_mirror_event(params)) {
		case ME_INSYNC:
			/* FIXME: all we really know is that this
			   _part_ of the device is in sync
			   Also, this is not an error
			*/
			syslog(LOG_NOTICE, "%s is now in-sync\n", device);
			break;
		case ME_FAILURE:
			syslog(LOG_ERR, "Device failure in %s\n", device);
			if (_remove_failed_devices(device))
				/* FIXME Why are all the error return codes unused? Get rid of them? */
				syslog(LOG_ERR, "Failed to remove faulty devices in %s\n",
				       device);
			/* Should check before warning user that device is now linear
			else
				syslog(LOG_NOTICE, "%s is now a linear device.\n",
					device);
			*/
			break;
		case ME_IGNORE:
			break;
		default:
			/* FIXME Wrong: it can also return -E2BIG but it's never used! */
			syslog(LOG_INFO, "Unknown event received.\n");
		}
	} while (next);

 fail:
	if (dmt)
		dm_task_destroy(dmt);
	pthread_mutex_unlock(&_event_mutex);
}

int register_device(const char *device)
{
	int r = 0;

	pthread_mutex_lock(&_register_mutex);

	syslog(LOG_INFO, "Monitoring mirror device, %s for events\n", device);

	/*
	 * Need some space for allocations.  1024 should be more
	 * than enough for what we need (device mapper name splitting)
	 */
	if (!_mem_pool && !(_mem_pool = dm_pool_create("mirror_dso", 1024)))
		goto out;

	if (!_lvm_handle) {
		lvm2_log_fn(_temporary_log_fn);
		if (!(_lvm_handle = lvm2_init())) {
			dm_pool_destroy(_mem_pool);
			_mem_pool = NULL;
			goto out;
		}
		lvm2_log_level(_lvm_handle, LVM2_LOG_SUPPRESS);
		/* FIXME Temporary: move to dmeventd core */
		lvm2_run(_lvm_handle, "_memlock_inc");
	}

	_register_count++;
	r = 1;

out:
	pthread_mutex_unlock(&_register_mutex);

	return r;
}

int unregister_device(const char *device)
{
	pthread_mutex_lock(&_register_mutex);

	if (!--_register_count) {
		dm_pool_destroy(_mem_pool);
		_mem_pool = NULL;
		lvm2_run(_lvm_handle, "_memlock_dec");
		lvm2_exit(_lvm_handle);
		_lvm_handle = NULL;
	}

	pthread_mutex_unlock(&_register_mutex);

	return 1;
}
