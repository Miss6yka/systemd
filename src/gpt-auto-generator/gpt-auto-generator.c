/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <blkid.h>

#include "path-util.h"
#include "util.h"
#include "mkdir.h"
#include "missing.h"
#include "sd-id128.h"
#include "libudev.h"
#include "special.h"
#include "unit-name.h"

/* TODO:
 *
 * - Properly handle cryptsetup partitions
 * - Define new partition type for encrypted swap
 * - Make /home automount rather than mount
 *
 */

static const char *arg_dest = "/tmp";

static int verify_gpt_partition(dev_t dev, sd_id128_t *type, unsigned *nr, char **fstype) {
        _cleanup_free_ char *t = NULL;
        blkid_probe b = NULL;
        const char *v;
        int r;

        r = asprintf(&t, "/dev/block/%u:%u", major(dev), minor(dev));
        if (r < 0)
                return -ENOMEM;

        errno = 0;
        b = blkid_new_probe_from_filename(t);
        if (!b) {
                if (errno != 0)
                        return -errno;

                return -ENOMEM;
        }

        blkid_probe_enable_superblocks(b, 1);
        blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE);
        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2) {
                r = -ENODEV;
                goto finish;
        } else if (r == 1) {
                r = -ENODEV;
                goto finish;
        } else if (r != 0) {
                r = errno ? -errno : -EIO;
                goto finish;
        }

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SCHEME", &v, NULL);
        if (r != 0) {
                r = errno ? -errno : -EIO;
                goto finish;
        }

        if (strcmp(v, "gpt") != 0) {
                r = 0;
                goto finish;
        }

        if (type) {
                errno = 0;
                r = blkid_probe_lookup_value(b, "PART_ENTRY_TYPE", &v, NULL);
                if (r != 0) {
                        r = errno ? -errno : -EIO;
                        goto finish;
                }

                r = sd_id128_from_string(v, type);
                if (r < 0)
                        return r;
        }

        if (nr) {
                errno = 0;
                r = blkid_probe_lookup_value(b, "PART_ENTRY_NUMBER", &v, NULL);
                if (r != 0) {
                        r = errno ? -errno : -EIO;
                        goto finish;
                }

                r = safe_atou(v, nr);
                if (r < 0)
                        return r;
        }


        if (fstype) {
                char *fst;

                errno = 0;
                r = blkid_probe_lookup_value(b, "TYPE", &v, NULL);
                if (r != 0)
                        *fstype = NULL;
                else {
                        fst = strdup(v);
                        if (!fst) {
                                r = -ENOMEM;
                                goto finish;
                        }

                        *fstype = fst;
                }
        }

        return 1;

finish:
        if (b)
                blkid_free_probe(b);

        return r;
}

static int add_swap(const char *path, const char *fstype) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        log_debug("Adding swap: %s %s", path, fstype);

        name = unit_name_from_path(path, ".swap");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "DefaultDependencies=no\n"
                "Conflicts=" SPECIAL_UMOUNT_TARGET "\n"
                "Before=" SPECIAL_UMOUNT_TARGET " " SPECIAL_SWAP_TARGET "\n\n"
                "[Mount]\n"
                "What=%s\n",
                path);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name, NULL);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0) {
                log_error("Failed to create symlink %s: %m", lnk);
                return -errno;
        }

        return 0;
}

static int add_home(const char *path, const char *fstype) {
        _cleanup_free_ char *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        if (dir_is_empty("/home") <= 0)
                return 0;

        log_debug("Adding home: %s %s", path, fstype);

        unit = strappend(arg_dest, "/home.mount");
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "DefaultDependencies=no\n"
                "After=" SPECIAL_LOCAL_FS_PRE_TARGET "\n"
                "Conflicts=" SPECIAL_UMOUNT_TARGET "\n"
                "Before=" SPECIAL_UMOUNT_TARGET " " SPECIAL_LOCAL_FS_TARGET "\n\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=/home\n"
                "Type=%s\n"
                "FsckPassNo=2\n",
                path, fstype);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        lnk = strjoin(arg_dest, "/" SPECIAL_LOCAL_FS_TARGET ".requires/home.mount", NULL);
        if (!lnk)
                return log_oom();


        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0) {
                log_error("Failed to create symlink %s: %m", lnk);
                return -errno;
        }

        return 0;
}

static int enumerate_partitions(dev_t dev) {
        struct udev *udev;
        struct udev_enumerate *e = NULL;
        struct udev_device *parent = NULL, *d = NULL;
        struct udev_list_entry *first, *item;
        unsigned home_nr = (unsigned) -1;
        _cleanup_free_ char *home = NULL, *home_fstype = NULL;
        int r;

        udev = udev_new();
        if (!udev)
                return log_oom();

        e = udev_enumerate_new(udev);
        if (!e) {
                r = log_oom();
                goto finish;
        }

        d = udev_device_new_from_devnum(udev, 'b', dev);
        if (!d) {
                r = log_oom();
                goto finish;
        }

        parent = udev_device_get_parent(d);
        if (!parent) {
                r = log_oom();
                goto finish;
        }

        r = udev_enumerate_add_match_parent(e, parent);
        if (r < 0) {
                r = log_oom();
                goto finish;
        }

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0) {
                r = log_oom();
                goto finish;
        }

        r = udev_enumerate_scan_devices(e);
        if (r < 0) {
                log_error("Failed to enumerate partitions: %s", strerror(-r));
                goto finish;
        }

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_free_ char *fstype = NULL;
                const char *node = NULL;
                struct udev_device *q;
                sd_id128_t type_id;
                unsigned nr;

                q = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!q) {
                        r = log_oom();
                        goto finish;
                }

                if (udev_device_get_devnum(q) == udev_device_get_devnum(d))
                        goto skip;

                if (udev_device_get_devnum(q) == udev_device_get_devnum(parent))
                        goto skip;

                node = udev_device_get_devnode(q);
                if (!node) {
                        r = log_oom();
                        goto finish;
                }

                r = verify_gpt_partition(udev_device_get_devnum(q), &type_id, &nr, &fstype);
                if (r < 0) {
                        log_error("Failed to verify GPT partition: %s", strerror(-r));
                        udev_device_unref(q);
                        goto finish;
                }
                if (r == 0)
                        goto skip;

                if (sd_id128_equal(type_id, SD_ID128_MAKE(06,57,fd,6d,a4,ab,43,c4,84,e5,09,33,c8,4b,4f,4f)))
                        add_swap(node, fstype);
                else if (sd_id128_equal(type_id, SD_ID128_MAKE(93,3a,c7,e1,2e,b4,4f,13,b8,44,0e,14,e2,ae,f9,15))) {

                        if (!home || nr < home_nr) {
                                free(home);
                                home = strdup(node);
                                if (!home) {
                                        r = log_oom();
                                        goto finish;
                                }

                                home_nr = nr;

                                free(home_fstype);
                                home_fstype = fstype;
                                fstype = NULL;
                        }
                }

        skip:
                udev_device_unref(q);
        }

        if (home && home_fstype)
                add_home(home, home_fstype);

finish:
        if (d)
                udev_device_unref(d);

        if (e)
                udev_enumerate_unref(e);

        if (udev)
                udev_unref(udev);

        return r;
}

static int get_btrfs_block_device(const char *path, dev_t *dev) {
        struct btrfs_ioctl_fs_info_args fsi;
        _cleanup_close_ int fd = -1;
        uint64_t id;

        assert(path);
        assert(dev);

        fd = open(path, O_DIRECTORY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        zero(fsi);
        if (ioctl(fd, BTRFS_IOC_FS_INFO, &fsi) < 0)
                return -errno;

        /* We won't do this for btrfs RAID */
        if (fsi.num_devices != 1)
                return 0;

        for (id = 1; id <= fsi.max_id; id++) {
                struct btrfs_ioctl_dev_info_args di;
                struct stat st;

                zero(di);
                di.devid = id;

                if (ioctl(fd, BTRFS_IOC_DEV_INFO, &di) < 0) {
                        if (errno == ENODEV)
                                continue;

                        return -errno;
                }

                if (stat((char*) di.path, &st) < 0)
                        return -errno;

                if (!S_ISBLK(st.st_mode))
                        return -ENODEV;

                if (major(st.st_rdev) == 0)
                        return -ENODEV;

                *dev = st.st_rdev;
                return 1;
        }

        return -ENODEV;
}

static int get_block_device(const char *path, dev_t *dev) {
        struct stat st;
        struct statfs sfs;

        assert(path);
        assert(dev);

        if (lstat("/", &st))
                return -errno;

        if (major(st.st_dev) != 0) {
                *dev = st.st_dev;
                return 1;
        }

        if (statfs("/", &sfs) < 0)
                return -errno;

        if (F_TYPE_CMP(sfs.f_type, BTRFS_SUPER_MAGIC))
                return get_btrfs_block_device(path, dev);

        return 0;
}

int main(int argc, char *argv[]) {
        dev_t dev;
        int r;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (in_initrd())
                return EXIT_SUCCESS;

        r = get_block_device("/", &dev);
        if (r < 0) {
                log_error("Failed to determine block device of root file system: %s", strerror(-r));
                return EXIT_FAILURE;
        }
        if (r == 0) {
                log_debug("Root file system not on a (single) block device.");
                return EXIT_SUCCESS;
        }

        log_debug("Root device %u:%u.", major(dev), minor(dev));

        r = verify_gpt_partition(dev, NULL, NULL, NULL);
        if (r < 0) {
                log_error("Failed to verify GPT partition: %s", strerror(-r));
                return EXIT_FAILURE;
        }
        if (r == 0)
                return EXIT_SUCCESS;

        r = enumerate_partitions(dev);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}