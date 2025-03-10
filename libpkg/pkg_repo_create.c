/*-
 * Copyright (c) 2011-2021 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2012-2013 Matthew Seaman <matthew@FreeBSD.org>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "pkg_config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/time.h>

#include <archive_entry.h>
#include <assert.h>
#include <fts.h>
#include <libgen.h>
#include <sqlite3.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sys/uio.h>
#include <msgpuck.h>

#include "pkg.h"
#include "private/event.h"
#include "private/utils.h"
#include "private/pkg.h"
#include "private/pkgdb.h"

enum {
	MSG_PKG_DONE=0,
	MSG_PKG_READY,
	MSG_DIGEST,
};

struct digest_list_entry {
	char *origin;
	char *digest;
	long manifest_pos;
	long files_pos;
	long manifest_length;
	char *checksum;
};
typedef tll(struct digest_list_entry *) digest_list_t;

struct pkg_conflict_bulk {
	struct pkg_conflict *conflicts;
	pkghash *conflictshash;
	char *file;
};

static int
hash_file(struct pkg_repo_meta *meta, struct pkg *pkg, char *path)
{
	char tmp_repo[MAXPATHLEN] = { 0 };
	char tmp_name[MAXPATHLEN] = { 0 };
	char repo_name[MAXPATHLEN] = { 0 };
	char hash_name[MAXPATHLEN] = { 0 };
	char link_name[MAXPATHLEN] = { 0 };
	char *rel_repo = NULL;
	char *rel_dir = NULL;
	char *rel_link = NULL;
	char *ext = NULL;

	/* Don't rename symlinks */
	if (is_link(path))
		return (EPKG_OK);

	ext = strrchr(path, '.');

	strlcpy(tmp_name, path, sizeof(tmp_name));
	rel_dir = get_dirname(tmp_name);
	while (strstr(rel_dir, "/Hashed") != NULL) {
		rel_dir = get_dirname(rel_dir);
	}
	strlcpy(tmp_name, rel_dir, sizeof(tmp_name));
	rel_dir = (char *)&tmp_name;

	rel_repo = path;
	if (strncmp(rel_repo, meta->repopath, strlen(meta->repopath)) == 0) {
		rel_repo += strlen(meta->repopath);
		while (rel_repo[0] == '/')
			rel_repo++;
	}
	strlcpy(tmp_repo, rel_repo, sizeof(tmp_repo));
	rel_repo = get_dirname(tmp_repo);
	while (strstr(rel_repo, "/Hashed") != NULL) {
		rel_repo = get_dirname(rel_repo);
	}
	strlcpy(tmp_repo, rel_repo, sizeof(tmp_repo));
	rel_repo = (char *)&tmp_repo;

	pkg_snprintf(repo_name, sizeof(repo_name), "%S/%S/%n-%v%S%z%S",
	    rel_repo, PKG_HASH_DIR, pkg, pkg, PKG_HASH_SEPSTR, pkg, ext);
	pkg_snprintf(link_name, sizeof(repo_name), "%S/%n-%v%S",
	    rel_dir, pkg, pkg, ext);
	pkg_snprintf(hash_name, sizeof(hash_name), "%S/%S/%n-%v%S%z%S",
	    rel_dir, PKG_HASH_DIR, pkg, pkg, PKG_HASH_SEPSTR, pkg, ext);
	rel_link = (char *)&hash_name;
	rel_link += strlen(rel_dir);
	while (rel_link[0] == '/')
		rel_link++;

	snprintf(tmp_name, sizeof(tmp_name), "%s/%s", rel_dir, PKG_HASH_DIR);
	rel_dir = (char *)&tmp_name;
	if (!is_dir(rel_dir)) {
		pkg_debug(1, "Making directory: %s", rel_dir);
		(void)mkdirs(rel_dir);
	}

	if (strcmp(path, hash_name) != 0) {
		pkg_debug(1, "Rename the pkg from: %s to: %s", path, hash_name);
		if (rename(path, hash_name) == -1) {
			pkg_emit_errno("rename", hash_name);
			return (EPKG_FATAL);
		}
	}
	if (meta->hash_symlink) {
		pkg_debug(1, "Symlinking pkg file from: %s to: %s", rel_link,
		    link_name);
		(void)unlink(link_name);
		if (symlink(rel_link, link_name) == -1) {
			pkg_emit_errno("symlink", link_name);
			return (EPKG_FATAL);
		}
	}
	free(pkg->repopath);
	pkg->repopath = xstrdup(repo_name);

	return (EPKG_OK);
}

static int
pkg_digest_sort_compare_func(struct digest_list_entry *d1,
		struct digest_list_entry *d2)
{
	return strcmp(d1->origin, d2->origin);
}

struct pkg_fts_item {
	char *fts_accpath;
	char *pkg_path;
	char *fts_name;
	off_t fts_size;
	int fts_info;
	struct pkg_fts_item *next;
};

static struct pkg_fts_item*
pkg_create_repo_fts_new(FTSENT *fts, const char *root_path)
{
	struct pkg_fts_item *item;
	char *pkg_path;

	item = xmalloc(sizeof(*item));
	item->fts_accpath = xstrdup(fts->fts_accpath);
	item->fts_name = xstrdup(fts->fts_name);
	item->fts_size = fts->fts_statp->st_size;
	item->fts_info = fts->fts_info;

	pkg_path = fts->fts_path;
	pkg_path += strlen(root_path);
	while (pkg_path[0] == '/')
		pkg_path++;

	item->pkg_path = xstrdup(pkg_path);

	return (item);
}

static void
pkg_create_repo_fts_free(struct pkg_fts_item *item)
{
	free(item->fts_accpath);
	free(item->pkg_path);
	free(item->fts_name);
	free(item);
}

static int
pkg_create_repo_read_fts(struct pkg_fts_item **items, FTS *fts,
	const char *repopath, size_t *plen, struct pkg_repo_meta *meta)
{
	FTSENT *fts_ent;
	struct pkg_fts_item *fts_cur;
	char *ext;
	int linklen = 0;
	char tmp_name[MAXPATHLEN] = { 0 };
	char repo_path[MAXPATHLEN];
	size_t repo_path_len;

	if (realpath(repopath, repo_path) == NULL) {
		pkg_emit_errno("invalid repo path", repopath);
		return (EPKG_FATAL);
	}
	repo_path_len = strlen(repo_path);
	errno = 0;

	while ((fts_ent = fts_read(fts)) != NULL) {
		/*
		 * Skip directories starting with '.' to avoid Poudriere
		 * symlinks.
		 */
		if ((fts_ent->fts_info == FTS_D ||
		    fts_ent->fts_info == FTS_DP) &&
		    fts_ent->fts_namelen > 2 &&
		    fts_ent->fts_name[0] == '.') {
			fts_set(fts, fts_ent, FTS_SKIP);
			continue;
		}
		/*
		 * Ignore 'Latest' directory as it is just symlinks back to
		 * already-processed packages.
		 */
		if ((fts_ent->fts_info == FTS_D ||
		    fts_ent->fts_info == FTS_DP ||
		    fts_ent->fts_info == FTS_SL) &&
		    strcmp(fts_ent->fts_name, "Latest") == 0) {
			fts_set(fts, fts_ent, FTS_SKIP);
			continue;
		}
		/* Follow symlinks. */
		if (fts_ent->fts_info == FTS_SL) {
			/*
			 * Skip symlinks pointing inside the repo
			 * and dead symlinks
			 */
			if (realpath(fts_ent->fts_path, tmp_name) == NULL)
				continue;
			if (strncmp(repo_path, tmp_name, repo_path_len) == 0)
				continue;
			/* Skip symlinks to hashed packages */
			if (meta->hash) {
				linklen = readlink(fts_ent->fts_path,
				    (char *)&tmp_name, MAXPATHLEN);
				if (linklen < 0)
					continue;
				tmp_name[linklen] = '\0';
				if (strstr(tmp_name, PKG_HASH_DIR) != NULL)
					continue;
			}
			fts_set(fts, fts_ent, FTS_FOLLOW);
			/* Restart. Next entry will be the resolved file. */
			continue;
		}
		/* Skip everything that is not a file */
		if (fts_ent->fts_info != FTS_F)
			continue;

		ext = strrchr(fts_ent->fts_name, '.');

		if (ext == NULL)
			continue;

		if (!packing_is_valid_format(ext + 1))
			continue;

		/* skip all files which are not .pkg */
		if (!ctx.repo_accept_legacy_pkg && strcmp(ext + 1, "pkg") != 0)
			continue;


		*ext = '\0';

		if (pkg_repo_meta_is_old_file(fts_ent->fts_name, meta)) {
			unlink(fts_ent->fts_path);
			continue;
		}
		if (strcmp(fts_ent->fts_name, "meta") == 0 ||
				pkg_repo_meta_is_special_file(fts_ent->fts_name, meta)) {
			*ext = '.';
			continue;
		}

		*ext = '.';
		fts_cur = pkg_create_repo_fts_new(fts_ent, repopath);
		if (fts_cur == NULL)
			return (EPKG_FATAL);

		LL_PREPEND(*items, fts_cur);
		(*plen) ++;
	}

	if (errno != 0) {
		pkg_emit_errno("fts_read", "pkg_create_repo_read_fts");
		return (EPKG_FATAL);
	}

	return (EPKG_OK);
}

static void
tell_parent(int fd, char *buf, size_t len)
{
	struct iovec iov[2];
	struct msghdr msg;

	iov[0].iov_base = buf;
	iov[0].iov_len = len;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	sendmsg(fd, &msg, MSG_EOR);
}

static int
pkg_create_repo_worker(int mfd, int ffd, int pip,
	struct pkg_repo_meta *meta)
{
	pid_t pid;
	struct pollfd *pfd = NULL;
	int flags, ret = EPKG_OK;
	size_t sz;
	struct pkg *pkg = NULL;
	struct pkg_manifest_key *keys = NULL;
	char *mdigest = NULL;
	char digestbuf[1024];
	xstring *b;
	struct iovec iov[2];
	uint32_t len;
	char buf[1024];
	char *w, *path;
	const char *rbuf, *c, *repopath;

	b = xstring_new();

	pid = fork();
	switch(pid) {
	case -1:
		pkg_emit_errno("pkg_create_repo_worker", "fork");
		xstring_free(b);
		return (EPKG_FATAL);
		break;
	case 0:
		break;
	default:
		/* Parent */
		xstring_free(b);
		return (EPKG_OK);
		break;
	}

	pkg_manifest_keys_new(&keys);
	pkg_debug(1, "start worker to parse packages");

	if (ffd != -1)
		flags = PKG_OPEN_MANIFEST_ONLY;
	else
		flags = PKG_OPEN_MANIFEST_ONLY | PKG_OPEN_MANIFEST_COMPACT;

	/* We are reading to digest buf but it's only to check the socketpair */
	if (read(pip, digestbuf, 1) == -1) {
		pkg_emit_errno("pkg_create_repo_worker", "read");
		goto cleanup;
	}

	pfd = xcalloc(1, sizeof(struct pollfd));
	pfd[0].fd = pip;
	pfd[0].events = POLLIN;

	for (;;) {
		w = buf;
		w = mp_encode_array(w, 1);
		w = mp_encode_uint(w, MSG_PKG_READY);
		tell_parent(pip, buf, w - buf);
		if (poll(pfd, 1, -1) == -1) {
			if (errno == EINTR)
				continue;
			else
				goto cleanup;
		}
		if (pfd[0].revents & (POLLIN|POLLHUP|POLLERR)) {
			for (;;) {
				ssize_t r;
				r = read(pfd[0].fd, buf, sizeof(buf));
				if (r == -1) {
					if (errno == EINTR)
						continue;
					else if (errno == EAGAIN || errno == EWOULDBLOCK) {
						return (EPKG_OK);
					}
					pkg_emit_errno("pkg_repo_worker", "read");
					return (EPKG_FATAL);
				} else if (r == 0)
					return (EPKG_END);
				else
					break;
			}
		} else {
			continue;
		}
		rbuf = buf;
		sz = mp_decode_array(&rbuf);
		if (sz < 1)
			continue;
		c = mp_decode_str(&rbuf, &len);
		if (len == 0) /* empty package name means ends of repo */
			break;
		path = xstrndup(c, len);
		repopath = mp_decode_str(&rbuf, &len);
		if (len == 0) /* empty package name means ends of repo */
			break;
		if (pkg_open(&pkg, path, keys, flags) == EPKG_OK) {
			off_t mpos, fpos = 0;
			size_t mlen;
			struct stat st;

			pkg->sum = pkg_checksum_file(path, PKG_HASH_TYPE_SHA256_HEX);
			stat(path, &st);
			pkg->pkgsize = st.st_size;
			if (meta->hash) {
				ret = hash_file(meta, pkg, path);
				if (ret != EPKG_OK)
					goto cleanup;
			} else {
				pkg->repopath = xstrdup(repopath);
			}

			/*
			 * TODO: use pkg_checksum for new manifests
			 */
			xstring_reset(b);
			mdigest = xmalloc(pkg_checksum_type_size(meta->digest_format));

			pkg_emit_manifest_buf(pkg, b, PKG_MANIFEST_EMIT_COMPACT, NULL);
			/* Only version 1 needs the digest */
			if (meta->version == 1) {
				if (pkg_checksum_generate(pkg, mdigest,
				    pkg_checksum_type_size(meta->digest_format),
				    meta->digest_format, false, true, false) != EPKG_OK) {
					pkg_emit_error("Cannot generate digest for a package");
					ret = EPKG_FATAL;

					goto cleanup;
				}
			}
			fflush(b->fp);
			mlen = strlen(b->buf);

			if (flock(mfd, LOCK_EX) == -1) {
				pkg_emit_errno("pkg_create_repo_worker", "flock");
				ret = EPKG_FATAL;
				goto cleanup;
			}

			mpos = lseek(mfd, 0, SEEK_END);

			iov[0].iov_base = b->buf;
			iov[0].iov_len = mlen;
			iov[1].iov_base = (void *)"\n";
			iov[1].iov_len = 1;

			if (writev(mfd, iov, 2) == -1) {
				pkg_emit_errno("pkg_create_repo_worker", "write");
				ret = EPKG_FATAL;
				flock(mfd, LOCK_UN);
				goto cleanup;
			}

			flock(mfd, LOCK_UN);

			if (ffd != -1) {
				FILE *fl;

				if (flock(ffd, LOCK_EX) == -1) {
					pkg_emit_errno("pkg_create_repo_worker", "flock");
					ret = EPKG_FATAL;
					goto cleanup;
				}
				fpos = lseek(ffd, 0, SEEK_END);
				fl = fdopen(dup(ffd), "a");
				pkg_emit_filelist(pkg, fl);
				fclose(fl);

				flock(ffd, LOCK_UN);
			}

			if (meta->version == 1) {
				w = buf;
				w = mp_encode_array(w, 7);
				w = mp_encode_uint(w, MSG_DIGEST);
				w = mp_encode_str(w, pkg->origin, strlen(pkg->origin));
				w = mp_encode_str(w, mdigest, strlen(mdigest));
				w = mp_encode_uint(w, mpos);
				w = mp_encode_uint(w, fpos);
				w = mp_encode_uint(w, mlen);
				w = mp_encode_str(w, pkg->sum, strlen(pkg->sum));
				tell_parent(pip, buf, w - buf);
			}
			/* send a tick */
			w = buf;
			w = mp_encode_array(w, 1);
			w = mp_encode_uint(w, MSG_PKG_DONE);
			tell_parent(pip, buf, w - buf);
		}
		free(path);
	}

cleanup:
	pkg_manifest_keys_free(keys);
	xstring_free(b);
	close(pip);
	free(mdigest);

	pkg_debug(1, "worker done");
	_exit(ret);
}

static int
pkg_create_repo_read_pipe(int fd, digest_list_t *dlist, struct pkg_fts_item **items)
{
	struct digest_list_entry *dig = NULL;
	char buf[1024];
	int r;
	size_t sz;
	uint32_t len;
	uint64_t msgtype;
	const char *rbuf;

	for (;;) {
		dig = NULL;
		r = read(fd, buf, sizeof(buf));
		if (r == -1) {
			if (errno == EINTR)
				continue;
			else if (errno == ECONNRESET) {
				/* Treat it as the end of a connection */
				return (EPKG_END);
			}
			else if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return (EPKG_OK);
			}

			pkg_emit_errno("pkg_create_repo_read_pipe", "read");
			return (EPKG_FATAL);
		}
		else if (r == 0)
			return (EPKG_END);

		rbuf = buf;
		sz = mp_decode_array(&rbuf);
		if (sz < 1)
			continue;
		msgtype = mp_decode_uint(&rbuf);

		if (msgtype == MSG_PKG_DONE) {
			return (EPKG_OK);
		}

		if (msgtype == MSG_DIGEST) {
			const char *c;
			dig = xcalloc(1, sizeof(*dig));
			c = mp_decode_str(&rbuf, &len);
			dig->origin = xstrndup(c, len);
			c = mp_decode_str(&rbuf, &len);
			dig->digest = xstrndup(c, len);
			dig->manifest_pos = mp_decode_uint(&rbuf);
			dig->files_pos = mp_decode_uint(&rbuf);
			dig->manifest_length = mp_decode_uint(&rbuf);
			c = mp_decode_str(&rbuf, &len);
			dig->checksum = xstrndup(c, len);
			tll_push_back(*dlist, dig);
		}

		if (msgtype == MSG_PKG_READY) {
			char *w;
			const char *str = (*items) != NULL ? (*items)->fts_accpath : "";
			const char *str2 = (*items) != NULL ? (*items)->pkg_path : "";
			w = buf;
			w = mp_encode_array(w, 2);
			w = mp_encode_str(w, str, strlen(str));
			w = mp_encode_str(w, str2, strlen(str2) + 1);
			if (*items != NULL)
				LL_DELETE((*items), (*items));
			tell_parent(fd, buf, w - buf);
		}
	}

	/*
	 * Never reached
	 */
	return (EPKG_OK);
}

#ifdef __linux__
typedef const FTSENT *FTSENTP;
#else
typedef const FTSENT *const FTSENTP;
#endif

static int
fts_compare(FTSENTP *a, FTSENTP *b)
{
	/* Sort files before directories, then alpha order */
	if ((*a)->fts_info != FTS_D && (*b)->fts_info == FTS_D)
		return -1;
	if ((*a)->fts_info == FTS_D && (*b)->fts_info != FTS_D)
		return 1;
	return (strcmp((*a)->fts_name, (*b)->fts_name));
}

int
pkg_create_repo(char *path, const char *output_dir, bool filelist,
	const char *metafile, bool hash, bool hash_symlink)
{
	FTS *fts = NULL;
	struct pkg_fts_item *fts_items = NULL;
	pkghash *conflicts = NULL;
	struct pkg_conflict_bulk *curcb;
	int num_workers, i, remaining_workers;
	size_t len, ntask;
	digest_list_t dlist = tll_init();
	struct pollfd *pfd = NULL;
	int cur_pipe[2], fd, outputdir_fd, mfd, ffd;
	struct pkg_repo_meta *meta = NULL;
	int retcode = EPKG_FATAL;
	ucl_object_t *meta_dump;
	FILE *mfile;
	pkghash_it it;

	char *repopath[2];
	char repodb[MAXPATHLEN];
	FILE *mandigests = NULL;

	mfd = ffd = -1;

	if (!is_dir(path)) {
		pkg_emit_error("%s is not a directory", path);
		return (EPKG_FATAL);
	}

	errno = 0;
	if (!is_dir(output_dir)) {
		/* Try to create dir */
		if (errno == ENOENT) {
			if (mkdir(output_dir, 00755) == -1) {
				pkg_fatal_errno("cannot create output directory %s",
					output_dir);
			}
		}
		else {
			pkg_emit_error("%s is not a directory", output_dir);
			return (EPKG_FATAL);
		}
	}
	if ((outputdir_fd = open(output_dir, O_DIRECTORY)) == -1) {
		pkg_emit_error("Cannot open %s", output_dir);
		return (EPKG_FATAL);
	}

	if (metafile != NULL) {
		fd = open(metafile, O_RDONLY);
		if (fd == -1) {
			pkg_emit_error("meta loading error while trying %s", metafile);
			return (EPKG_FATAL);
		}
		if (pkg_repo_meta_load(fd, &meta) != EPKG_OK) {
			pkg_emit_error("meta loading error while trying %s", metafile);
			close(fd);
			return (EPKG_FATAL);
		}
		close(fd);
	} else {
		meta = pkg_repo_meta_default();
	}
	meta->repopath = path;
	meta->hash = hash;
	meta->hash_symlink = hash_symlink;

	repopath[0] = path;
	repopath[1] = NULL;

	num_workers = pkg_object_int(pkg_config_get("WORKERS_COUNT"));
	if (num_workers <= 0) {
		num_workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (num_workers == -1)
			num_workers = 6;
	}

	if ((fts = fts_open(repopath, FTS_PHYSICAL|FTS_NOCHDIR, fts_compare)) == NULL) {
		pkg_emit_errno("fts_open", path);
		goto cleanup;
	}

	if ((mfd = openat(outputdir_fd, meta->manifests,
	     O_CREAT|O_TRUNC|O_WRONLY, 00644)) == -1) {
		goto cleanup;
	}
	if (filelist) {
		if ((ffd = openat(outputdir_fd, meta->filesite,
		        O_CREAT|O_TRUNC|O_WRONLY, 00644)) == -1) {
			goto cleanup;
		}
	}
	if (meta->version == 1) {
		if ((fd = openat(outputdir_fd, meta->digests, O_CREAT|O_TRUNC|O_RDWR, 00644)) == -1) {
			goto cleanup;
		}
		if ((mandigests = fdopen(fd, "w")) == NULL) {
			goto cleanup;
		}
	}

	len = 0;

	pkg_create_repo_read_fts(&fts_items, fts, path, &len, meta);

	if (len == 0) {
		/* Nothing to do */
		pkg_emit_error("No package files have been found");
		goto cleanup;
	}

	/* Split items over all workers */
	num_workers = MIN(num_workers, len);

	/* Launch workers */
	pkg_emit_progress_start("Creating repository in %s", output_dir);

	pfd = xcalloc(num_workers, sizeof(struct pollfd));
	ntask = 0;

	for (int i = 0; i < num_workers; i++) {
		/* Create new worker */
		int ofl;

		if (get_socketpair(cur_pipe) == -1) {
			pkg_emit_errno("pkg_create_repo", "pipe");
			goto cleanup;
		}

		if (pkg_create_repo_worker(mfd,
		    ffd, cur_pipe[1], meta) == EPKG_FATAL) {
			close(cur_pipe[0]);
			close(cur_pipe[1]);
			goto cleanup;
		}

		pfd[i].fd = cur_pipe[0];
		pfd[i].events = POLLIN;
		close(cur_pipe[1]);
		/* Make our end of the pipe non-blocking */
		ofl = fcntl(cur_pipe[0], F_GETFL, 0);
		fcntl(cur_pipe[0], F_SETFL, ofl | O_NONBLOCK);
	}

	/* Send start marker to all workers */
	for (i = 0; i < num_workers; i ++) {
		if (write(pfd[i].fd, ".", 1) == -1)
			pkg_emit_errno("pkg_create_repo", "write");
	}

	ntask = 0;
	remaining_workers = num_workers;
	while(remaining_workers > 0) {
		int st;

		pkg_debug(1, "checking for %d workers", remaining_workers);
		retcode = poll(pfd, num_workers, -1);
		if (retcode == -1) {
			if (errno == EINTR) {
				continue;
			}
			else {
				goto cleanup;
			}
		}
		else if (retcode > 0) {
			for (i = 0; i < num_workers; i ++) {
				if (pfd[i].fd != -1 &&
								(pfd[i].revents & (POLLIN|POLLHUP|POLLERR))) {
					if (pkg_create_repo_read_pipe(pfd[i].fd, &dlist, &fts_items) != EPKG_OK) {
						/*
						 * Wait for the worker finished
						 */
						while (wait(&st) == -1) {
							if (errno == EINTR)
								continue;

							pkg_emit_errno("pkg_create_repo", "wait");
							break;
						}

						remaining_workers --;
						pkg_debug(1, "finished worker, %d remaining",
							remaining_workers);
						pfd[i].events = 0;
						pfd[i].revents = 0;
						close(pfd[i].fd);
						pfd[i].fd = -1;
					} else {
						if (errno == EAGAIN || errno == EWOULDBLOCK) {
							errno = 0;
							continue;
						}
						pkg_emit_progress_tick(ntask++, len);
					}
				}
			}
		}
	}

	pkg_emit_progress_tick(len, len);

	/* Now sort all digests */
	if (meta->version == 1)
		tll_sort(dlist, pkg_digest_sort_compare_func);

	/* Write metafile */
	snprintf(repodb, sizeof(repodb), "%s/%s", output_dir,
		"meta");
	if ((mfile = fopen(repodb, "we")) != NULL) {
		meta_dump = pkg_repo_meta_to_ucl(meta);
		ucl_object_emit_file(meta_dump, UCL_EMIT_CONFIG, mfile);
		fclose(mfile);
		strlcat(repodb, ".conf", sizeof(repodb));
		if ((mfile = fopen(repodb, "we")) != NULL) {
			ucl_object_emit_file(meta_dump, UCL_EMIT_CONFIG, mfile);
			fclose(mfile);
		} else {
			pkg_emit_notice("cannot create metafile at %s", repodb);
		}
		ucl_object_unref(meta_dump);
	}
	else {
		pkg_emit_notice("cannot create metafile at %s", repodb);
	}
	retcode = EPKG_OK;
cleanup:
	if (outputdir_fd != -1)
		close(outputdir_fd);
	if (mfd != -1)
		close(mfd);
	if (ffd != -1)
		close(ffd);
	it = pkghash_iterator(conflicts);
	while (pkghash_next(&it)) {
		curcb = (struct pkg_conflict_bulk *)it.value;
		LL_FREE(curcb->conflicts, pkg_conflict_free);
		pkghash_destroy(curcb->conflictshash);
		curcb->conflictshash = NULL;
		free(curcb);
	}
	pkghash_destroy(conflicts);

	if (pfd != NULL)
		free(pfd);
	if (fts != NULL)
		fts_close(fts);

	LL_FREE(fts_items, pkg_create_repo_fts_free);

	if (meta->version == 1) {
		tll_foreach(dlist, it) {
			if (it->item->checksum != NULL)
				fprintf(mandigests, "%s:%s:%ld:%ld:%ld:%s\n", it->item->origin,
				    it->item->digest, it->item->manifest_pos, it->item->files_pos,
				    it->item->manifest_length, it->item->checksum);
			else
				fprintf(mandigests, "%s:%s:%ld:%ld:%ld\n", it->item->origin,
				    it->item->digest, it->item->manifest_pos, it->item->files_pos,
				    it->item->manifest_length);

			free(it->item->digest);
			free(it->item->origin);
			free(it->item);
		}
	}

	tll_free(dlist);
	if (meta->version == 1 && mandigests != NULL)
		fclose(mandigests);
	pkg_repo_meta_free(meta);

	return (retcode);
}

static int
pkg_repo_sign(char *path, char **argv, int argc, char **sig, size_t *siglen,
    char **cert)
{
	FILE *fp;
	char *sha256;
	xstring *cmd = NULL;
	xstring *buf = NULL;
	xstring *sigstr = NULL;
	xstring *certstr = NULL;
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int i, ret = EPKG_OK;

	sha256 = pkg_checksum_file(path, PKG_HASH_TYPE_SHA256_HEX);
	if (!sha256)
		return (EPKG_FATAL);

	cmd = xstring_new();

	for (i = 0; i < argc; i++) {
		if (strspn(argv[i], " \t\n") > 0)
			fprintf(cmd->fp, " \"%s\" ", argv[i]);
		else
			fprintf(cmd->fp, " %s ", argv[i]);
	}

	fflush(cmd->fp);
	if ((fp = popen(cmd->buf, "r+")) == NULL) {
		ret = EPKG_FATAL;
		goto done;
	}

	fprintf(fp, "%s\n", sha256);

	sigstr = xstring_new();
	certstr = xstring_new();

	while ((linelen = getline(&line, &linecap, fp)) > 0 ) {
		if (strcmp(line, "SIGNATURE\n") == 0) {
			buf = sigstr;
			continue;
		} else if (strcmp(line, "CERT\n") == 0) {
			buf = certstr;
			continue;
		} else if (strcmp(line, "END\n") == 0) {
			break;
		}
		if (buf != NULL) {
			fwrite(line, linelen, 1, buf->fp);
		}
	}

	*cert = xstring_get(certstr);
	fclose(sigstr->fp);
	sigstr->size--;
	*siglen = sigstr->size;
	*sig = sigstr->buf;
	free(sigstr);

	/* remove the latest \n */

	if (pclose(fp) != 0) {
		ret = EPKG_FATAL;
		goto done;
	}

done:
	free(sha256);
	xstring_free(cmd);

	return (ret);
}

static int
pkg_repo_pack_db(const char *name, const char *archive, char *path,
		struct pkg_key *keyinfo, struct pkg_repo_meta *meta,
		char **argv, int argc)
{
	struct packing *pack;
	unsigned char *sigret = NULL;
	unsigned int siglen = 0;
	size_t signature_len = 0;
	char fname[MAXPATHLEN];
	char *sig, *pub;
	int ret = EPKG_OK;

	sig = NULL;
	pub = NULL;

	if (packing_init(&pack, archive, meta->packing_format, 0, (time_t)-1, true, true) != EPKG_OK)
		return (EPKG_FATAL);

	if (keyinfo != NULL) {
		if (rsa_sign(path, keyinfo, &sigret, &siglen) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto out;
		}

		if (packing_append_buffer(pack, sigret, "signature", siglen + 1) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto out;
		}
	} else if (argc >= 1) {
		if (pkg_repo_sign(path, argv, argc, &sig, &signature_len, &pub) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto out;
		}

		snprintf(fname, sizeof(fname), "%s.sig", name);
		if (packing_append_buffer(pack, sig, fname, signature_len) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto out;
		}

		snprintf(fname, sizeof(fname), "%s.pub", name);
		if (packing_append_buffer(pack, pub, fname, strlen(pub)) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto out;
		}

	}
	packing_append_file_attr(pack, path, name, "root", "wheel", 0644, 0);

out:
	packing_finish(pack);
	unlink(path);
	free(sigret);
	free(sig);
	free(pub);

	return (ret);
}

int
pkg_finish_repo(const char *output_dir, pkg_password_cb *password_cb,
    char **argv, int argc, bool filelist)
{
	char repo_path[MAXPATHLEN];
	char repo_archive[MAXPATHLEN];
	char *key_file;
	const char *key_type;
	struct pkg_key *keyinfo = NULL;
	struct pkg_repo_meta *meta;
	struct stat st;
	int ret = EPKG_OK, nfile = 0, fd;
	const int files_to_pack = 4;

	if (!is_dir(output_dir)) {
		pkg_emit_error("%s is not a directory", output_dir);
		return (EPKG_FATAL);
	}

	if (argc == 1) {
		key_type = key_file = argv[0];
		if (strncmp(key_file, "rsa:", 4) == 0) {
			key_file += 4;
			*(key_file - 1) = '\0';
		} else {
			key_type = "rsa";
		}

		pkg_debug(1, "Loading %s key from '%s' for signing", key_type, key_file);
		rsa_new(&keyinfo, password_cb, key_file);
	}

	if (argc > 1 && strcmp(argv[0], "signing_command:") != 0)
		return (EPKG_FATAL);

	if (argc > 1) {
		argc--;
		argv++;
	}

	pkg_emit_progress_start("Packing files for repository");
	pkg_emit_progress_tick(nfile++, files_to_pack);

	snprintf(repo_path, sizeof(repo_path), "%s/%s", output_dir,
		repo_meta_file);
	if ((fd = open(repo_path, O_RDONLY)) != -1) {
		if (pkg_repo_meta_load(fd, &meta) != EPKG_OK) {
			pkg_emit_error("meta loading error while trying %s", repo_path);
			rsa_free(keyinfo);
			close(fd);
			return (EPKG_FATAL);
		}
		if (pkg_repo_pack_db(repo_meta_file, repo_path, repo_path, keyinfo,
		    meta, argv, argc) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto cleanup;
		}
	}
	else {
		meta = pkg_repo_meta_default();
	}

	snprintf(repo_path, sizeof(repo_path), "%s/%s", output_dir,
	    meta->manifests);
	snprintf(repo_archive, sizeof(repo_archive), "%s/%s", output_dir,
		meta->manifests_archive);
	if (pkg_repo_pack_db(meta->manifests, repo_archive, repo_path, keyinfo,
	    meta, argv, argc) != EPKG_OK) {
		ret = EPKG_FATAL;
		goto cleanup;
	}

	pkg_emit_progress_tick(nfile++, files_to_pack);

	if (filelist) {
		snprintf(repo_path, sizeof(repo_path), "%s/%s", output_dir,
		    meta->filesite);
		snprintf(repo_archive, sizeof(repo_archive), "%s/%s",
		    output_dir, meta->filesite_archive);
		if (pkg_repo_pack_db(meta->filesite, repo_archive, repo_path, keyinfo,
		    meta, argv, argc) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto cleanup;
		}
	}

	pkg_emit_progress_tick(nfile++, files_to_pack);

	if (meta->version == 1) {
		snprintf(repo_path, sizeof(repo_path), "%s/%s", output_dir,
		    meta->digests);
		snprintf(repo_archive, sizeof(repo_archive), "%s/%s", output_dir,
		    meta->digests_archive);
		if (pkg_repo_pack_db(meta->digests, repo_archive, repo_path, keyinfo,
		    meta, argv, argc) != EPKG_OK) {
			ret = EPKG_FATAL;
			goto cleanup;
		}
	}

	pkg_emit_progress_tick(nfile++, files_to_pack);

#if 0
	snprintf(repo_path, sizeof(repo_path), "%s/%s", output_dir,
		meta->conflicts);
	snprintf(repo_archive, sizeof(repo_archive), "%s/%s", output_dir,
		meta->conflicts_archive);
	if (pkg_repo_pack_db(meta->conflicts, repo_archive, repo_path, keyinfo,
	    meta, argv, argc) != EPKG_OK) {
		ret = EPKG_FATAL;
		goto cleanup;
	}
#endif

	/* Now we need to set the equal mtime for all archives in the repo */
	snprintf(repo_archive, sizeof(repo_archive), "%s/%s.pkg",
	    output_dir, repo_meta_file);
	if (stat(repo_archive, &st) == 0) {
		struct timeval ftimes[2] = {
			{
			.tv_sec = st.st_mtime,
			.tv_usec = 0
			},
			{
			.tv_sec = st.st_mtime,
			.tv_usec = 0
			}
		};
		snprintf(repo_archive, sizeof(repo_archive), "%s/%s.pkg",
		    output_dir, meta->manifests_archive);
		utimes(repo_archive, ftimes);
		if (meta->version == 1) {
			snprintf(repo_archive, sizeof(repo_archive), "%s/%s.pkg",
			    output_dir, meta->digests_archive);
			utimes(repo_archive, ftimes);
		}
		if (filelist) {
			snprintf(repo_archive, sizeof(repo_archive),
			    "%s/%s.pkg", output_dir, meta->filesite_archive);
			utimes(repo_archive, ftimes);
		}
		snprintf(repo_archive, sizeof(repo_archive),
			"%s/%s.pkg", output_dir, repo_meta_file);
		utimes(repo_archive, ftimes);
	}

cleanup:
	pkg_emit_progress_tick(files_to_pack, files_to_pack);
	pkg_repo_meta_free(meta);

	rsa_free(keyinfo);

	return (ret);
}
