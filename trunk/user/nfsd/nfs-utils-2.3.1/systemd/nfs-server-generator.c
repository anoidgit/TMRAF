/*
 * nfs-server-generator:
 *   systemd generator to create ordering dependencies between
 *   nfs-server and various filesystem mounts
 *
 * 1/ nfs-server should start Before any 'nfs' mountpoints are
 *    mounted, in case they are loop-back mounts.  This ordering is particularly
 *    important for the shutdown side, so the nfs-server is stopped
 *    after the filesystems are unmounted.
 * 2/ nfs-server should start After all exported filesystems are mounted
 *    so there is no risk of exporting the underlying directory.
 *    This is particularly important for _net mounts which
 *    are not caught by "local-fs.target".
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <mntent.h>

#include "misc.h"
#include "nfslib.h"
#include "exportfs.h"
#include "systemd.h"

/* A simple "set of strings" to remove duplicates
 * found in /etc/exports
 */
struct list {
	struct list *next;
	char *name;
};
static int is_unique(struct list **lp, char *path)
{
	struct list *l = *lp;

	while (l) {
		if (strcmp(l->name, path) == 0)
			return 0;
		l = l->next;
	}
	l = malloc(sizeof(*l));
	if (l == NULL)
		return 0;
	l->name = path;
	l->next = *lp;
	*lp = l;
	return 1;
}

static int has_noauto_flag(char *path)
{
	FILE		*fstab;
	struct mntent	*mnt;

	fstab = setmntent("/etc/fstab", "r");
	if (!fstab)
		return 0;

	while ((mnt = getmntent(fstab)) != NULL) {
		int l = strlen(mnt->mnt_dir);
		if (strncmp(mnt->mnt_dir, path, l) != 0)
			continue;
		if (path[l] && path[l] != '/')
			continue;
		if (hasmntopt(mnt, "noauto"))
			break;
	}
	fclose(fstab);
	return mnt != NULL;
}

int main(int argc, char *argv[])
{
	char		*path, *spath;
	char		dirbase[] = "/nfs-server.service.d";
	char		filebase[] = "/order-with-mounts.conf";
	nfs_export	*exp;
	int		i;
	struct list	*list = NULL;
	FILE		*f, *fstab;
	struct mntent	*mnt;

	/* Avoid using any external services */
	xlog_syslog(0);

	if (argc != 4 || argv[1][0] != '/') {
		fprintf(stderr, "nfs-server-generator: create systemd dependencies for nfs-server\n");
		fprintf(stderr, "Usage: normal-dir early-dir late-dir\n");
		exit(1);
	}

	path = malloc(strlen(argv[1]) + sizeof(dirbase) + sizeof(filebase));
	if (!path)
		exit(2);
	if (export_read(_PATH_EXPORTS, 1) +
	    export_d_read(_PATH_EXPORTS_D, 1) == 0)
		/* Nothing is exported, so nothing to do */
		exit(0);

	strcat(strcpy(path, argv[1]), dirbase);
	mkdir(path, 0755);
	strcat(path, filebase);
	f = fopen(path, "w");
	if (!f)
		exit(1);
	fprintf(f, "# Automatically generated by nfs-server-generator\n\n[Unit]\n");

	for (i = 0; i < MCL_MAXTYPES; i++) {
		for (exp = exportlist[i].p_head; exp; exp = exp->m_next) {
			if (!is_unique(&list, exp->m_export.e_path))
				continue;
			if (exp->m_export.e_mountpoint)
				continue;
			if (has_noauto_flag(exp->m_export.e_path))
				continue;
			if (strchr(exp->m_export.e_path, ' '))
				fprintf(f, "RequiresMountsFor=\"%s\"\n",
					exp->m_export.e_path);
			else
				fprintf(f, "RequiresMountsFor=%s\n",
					exp->m_export.e_path);
		}
	}

	fstab = setmntent("/etc/fstab", "r");
	if (!fstab)
		exit(1);

	while ((mnt = getmntent(fstab)) != NULL) {
		if (strcmp(mnt->mnt_type, "nfs") != 0 &&
		    strcmp(mnt->mnt_type, "nfs4") != 0)
			continue;

		spath = systemd_escape(mnt->mnt_dir, ".mount");
		if (!spath) {
			fprintf(stderr, 
				"nfs-server-generator: convert path failed: %s\n",
				mnt->mnt_dir);
			continue;
		}
		fprintf(f, "Before=%s\n", spath);
	}

	fclose(fstab);
	fclose(f);

	exit(0);
}
