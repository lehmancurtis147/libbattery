/* -*- linux-c -*-
 *
 * Distribute under LGPL v2 or (at your option) later version.
 *
 * Copyright (C) 2017 Pavel Machek <pavel@ucw.cz>
 *
 * Thanks to Sam Lantinga and his SDL2.
 * Thanks to Marek Belisko.
 */

#include <stdio.h>
#include <string.h>
#include <alloca.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <dirent.h>

#include "battery.h"

static const char *sys_class_power_supply_path = "/sys/class/power_supply";

static int
open_power_file(const char *base, const char *node, const char *key)
{
	const size_t pathlen = strlen(base) + strlen(node) + strlen(key) + 3;
	char *path = (char *) alloca(pathlen);
	if (path == NULL) {
		return -1;  /* oh well. */
	}

	snprintf(path, pathlen, "%s/%s/%s", base, node, key);
	return open(path, O_RDONLY);
}

static bool
read_power_file(const char *base, const char *node, const char *key,
                char *buf, size_t buflen)
{
	ssize_t br = 0;
	const int fd = open_power_file(base, node, key);
	if (fd == -1) {
		return false;
	}
	br = read(fd, buf, buflen-1);
	close(fd);
	if (br < 0) {
		return false;
	}
	buf[br] = '\0';             /* null-terminate the string. */
	return true;
}

static bool
int_string(char *str, int *val)
{
	char *endptr = NULL;
	*val = (int) strtol(str, &endptr, 0);
	return ((*str != '\0') && (*endptr == '\0'));
}

/* calculate remaining fuel level (in %) of a LiIon battery assuming
 * a standard chemistry model
 *    The first reference found on the web seems to be a forum post
 *    by "SilverFox" from 04-16-2008. It appears to be attributed to Sanyo.
 *    http://www.candlepowerforums.com/vb/showthread.php?115871-Li-Ion-State-of-Charge-and-Voltage-Measurements#post2440539
 *    The linear interpplation below 19.66% was suggested by Pavel Machek.
 *
 * @mV: voltage measured outside the battery
 * @mA: current flowing out of the battery
 * @mOhm: assumed series resitance of the battery
 *
 * returns value between 0 and 100
 */
static inline int fuel_level_LiIon(int mV, int mA, int mOhm)
{
	int u;

	/* internal battery voltage is higher than measured when discharging */
	mV += (mOhm * mA) /1000;

	/* apply first part of formula */
	u = 3870000 - (14523 * (37835 - 10 * mV));

	/* use linear approx. below 3.756V => 19.66% assuming 3.3V => 0% */
	if (u < 0) {
		return  max(((mV - 3300) * ((3756 - 3300) * 1966)) / 100000, 0);
	}

	/* apply second part of formula */
	return min((int)(1966 + int_sqrt(u))/100, 100);
}


bool
GetPowerInfo_Linux_sys_class_power_supply(enum battery_state *state, int *seconds, int *percent)
{
	const char *base = sys_class_power_supply_path;
	struct dirent *dent;
	DIR *dirp;

	dirp = opendir(base);
	if (!dirp) {
		return false;
	}

	*state = POWERSTATE_NO_BATTERY;  /* assume we're just plugged in. */
	*seconds = -1;
	*percent = -1;

	while ((dent = readdir(dirp)) != NULL) {
		const char *name = dent->d_name;
		bool choose = false;
		char str[64];
		enum battery_state st;
		int secs;
		int pct;

		if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
			continue;  /* skip these, of course. */
		} else if (!read_power_file(base, name, "type", str, sizeof (str))) {
			continue;  /* Don't know _what_ we're looking at. Give up on it. */
		} else if (strcmp(str, "Battery\n") != 0) {
			continue;  /* we don't care about UPS and such. */
		}

		/* if the scope is "device," it might be something like a PS4
		   controller reporting its own battery, and not something that powers
		   the system. Most system batteries don't list a scope at all; we
		   assume it's a system battery if not specified. */
		if (read_power_file(base, name, "scope", str, sizeof (str))) {
			if (strcmp(str, "device\n") == 0) {
				continue;  /* skip external devices with their own batteries. */
			}
		}

		/* some drivers don't offer this, so if it's not explicitly reported assume it's present. */
		if (read_power_file(base, name, "present", str, sizeof (str)) && (strcmp(str, "0\n") == 0)) {
			st = POWERSTATE_NO_BATTERY;
		} else if (!read_power_file(base, name, "status", str, sizeof (str))) {
			st = POWERSTATE_UNKNOWN;  /* uh oh */
		} else if (strcmp(str, "Charging\n") == 0) {
			st = POWERSTATE_CHARGING;
		} else if (strcmp(str, "Discharging\n") == 0) {
			st = POWERSTATE_ON_BATTERY;
		} else if ((strcmp(str, "Full\n") == 0) || (strcmp(str, "Not charging\n") == 0)) {
			st = POWERSTATE_CHARGED;
		} else {
			st = POWERSTATE_UNKNOWN;  /* uh oh */
		}

		if (!read_power_file(base, name, "capacity", str, sizeof (str))) {
			pct = -1;
		} else {
			pct = atoi(str);
			pct = (pct > 100) ? 100 : pct; /* clamp between 0%, 100% */
		}

		if (!read_power_file(base, name, "time_to_empty_now", str, sizeof (str))) {
			secs = -1;
		} else {
			secs = atoi(str);
			secs = (secs <= 0) ? -1 : secs;  /* 0 == unknown */
		}

		/*
		 * We pick the battery that claims to have the most minutes left.
		 *  (failing a report of minutes, we'll take the highest percent.)
		 */
		if ((secs < 0) && (*seconds < 0)) {
			if ((pct < 0) && (*percent < 0)) {
				choose = true;  /* at least we know there's a battery. */
			} else if (pct > *percent) {
				choose = true;
			}
		} else if (secs > *seconds) {
			choose = true;
		}

		if (choose) {
			*seconds = secs;
			*percent = pct;
			*state = st;
		}
	}

	closedir(dirp);
	return true;  /* don't look any further. */
}

struct battery *battery_init(void)
{
	return NULL;
}

double battery_percent(struct battery *b)
{
	return -1;
}