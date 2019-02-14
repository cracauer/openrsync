/*	$Id$ */
/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/stat.h>

#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "md4.h"
#include "extern.h"

/*
 * Flush out "size" bytes of the buffer, doing all of the appropriate
 * chunking of the data, then the subsequent token (or zero).
 * Return zero on failure, non-zero on success.
 */
static int
blk_flush(struct sess *sess, int fd,
	const void *b, off_t size, int32_t token)
{
	off_t	i = 0, sz;

	while (i < size) {
		sz = MAX_CHUNK < (size - i) ?
			MAX_CHUNK : (size - i);
		if (!io_write_int(sess, fd, sz)) {
			ERRX1(sess, "io_write_int");
			return 0;
		} else if (!io_write_buf(sess, fd, b + i, sz)) {
			ERRX1(sess, "io_write_buf");
			return 0;
		}
		i += sz;
	}

	if (!io_write_int(sess, fd, token)) {
		ERRX1(sess, "io_write_int");
		return 0;
	}

	return 1;
}

/*
 * From our current position of "offs" in buffer "buf" of total size
 * "size", see if we can find a matching block in our list of blocks.
 * The "hint" refers to the block that *might* work.
 * Returns the blk or NULL if no matching block was found.
 */
static struct blk *
blk_find(struct sess *sess, const void *buf, off_t size, off_t offs,
	const struct blkset *blks, const char *path, size_t hint)
{
	unsigned char	 md[MD4_DIGEST_LENGTH];
	uint32_t	 fhash;
	off_t		 remain, osz;
	size_t		 i;
	int		 have_md = 0;

	/*
	 * First, compute our fast hash.
	 * FIXME: yes, this can be a rolling computation, but I'm
	 * deliberately making it simple first.
	 */

	remain = size - offs;
	assert(remain);
	osz = remain < (off_t)blks->len ? remain : (off_t)blks->len;
	fhash = hash_fast(buf + offs, (size_t)osz);
	have_md = 0;

	/*
	 * Start with our match hint.
	 * This just runs the fast and slow check with the hint.
	 */

	if (hint < blks->blksz &&
	    fhash == blks->blks[hint].chksum_short &&
	    (size_t)osz == blks->blks[hint].len) {
		hash_slow(buf + offs, (size_t)osz, md, sess);
		have_md = 1;
		if (memcmp(md, blks->blks[hint].chksum_long, blks->csum) == 0) {
			LOG4(sess, "%s: found matching hinted match: "
				"position %jd, block %zu "
				"(position %jd, size %zu)", path,
				(intmax_t)offs, blks->blks[hint].idx,
				(intmax_t)blks->blks[hint].offs,
				blks->blks[hint].len);
			return &blks->blks[hint];
		}
	}

	/*
	 * Now loop and look for the fast hash.
	 * If it's found, move on to the slow hash.
	 */

	for (i = 0; i < blks->blksz; i++) {
		if (fhash != blks->blks[i].chksum_short)
			continue;
		if ((size_t)osz != blks->blks[i].len)
			continue;

		LOG4(sess, "%s: found matching fast match: "
			"position %jd, block %zu "
			"(position %jd, size %zu)", path,
			(intmax_t)offs, blks->blks[i].idx,
			(intmax_t)blks->blks[i].offs,
			blks->blks[i].len);

		/* Compute slow hash on demand. */

		if (have_md == 0) {
			hash_slow(buf + offs, (size_t)osz, md, sess);
			have_md = 1;
		}

		if (memcmp(md, blks->blks[i].chksum_long, blks->csum))
			continue;

		LOG4(sess, "%s: sender verifies slow match", path);
		return &blks->blks[i];
	}

	return NULL;
}

/*
 * The main reconstruction algorithm on the sender side.
 * This is reentrant: it's meant to be called whenever "fd" unblocks for
 * writing by the sender.
 * Scans byte-wise over the input file, looking for matching blocks in
 * what the server sent us.
 * If a block is found, emit all data up until the block, then the token
 * for the block.
 * The receiving end can then reconstruct the file trivially.
 * Return zero on failure, non-zero on success.
 */
static int
blk_match_send(struct sess *sess, const char *path, int fd,
	const struct blkset *blks, struct blkstat *st)
{
	off_t		 last, end, sz;
	int32_t		 tok;
	struct blk	*blk;

	/*
	 * Stop searching at the length of the file minus the size of
	 * the last block.
	 * The reason for this being that we don't need to do an
	 * incremental hash within the last block---if it doesn't match,
	 * it doesn't match.
	 */

	end = st->mapsz + 1 - blks->blks[blks->blksz - 1].len;
	last = st->offs;

	for ( ; st->offs < end; st->offs++) {
		blk = blk_find(sess, st->map, st->mapsz,
			st->offs, blks, path, st->hint);
		if (blk == NULL)
			continue;

		sz = st->offs - last;
		st->dirty += sz;
		st->total += sz;
		LOG4(sess, "%s: flushing %jd B before %zu B "
			"block %zu", path, (intmax_t)sz, blk->len, blk->idx);
		tok = -(blk->idx + 1);

		/*
		 * Write the data we have, then follow it with the tag
		 * of the block that matches.
		 * The receiver will then write our data, then the data
		 * it already has in the matching block.
		 */

		if (!blk_flush(sess, fd, st->map + last, sz, tok)) {
			ERRX1(sess, "blk_flush");
			return -1;
		}

		st->total += blk->len;
		st->offs += blk->len;
		st->hint = blk->idx + 1;
		return 0;
	}

	/* Emit remaining data and send terminator token. */

	sz = st->mapsz - last;
	st->total += sz;
	st->dirty += sz;

	LOG4(sess, "%s: flushing remaining %jd B", path, (intmax_t)sz);

	if (!blk_flush(sess, fd, st->map + last, sz, 0)) {
		ERRX1(sess, "blk_flush");
		return -1;
	}

	LOG3(sess, "%s: flushed (chunked) %jd B total, "
		"%.2f%% upload ratio", path, (intmax_t)st->total,
		100.0 * st->dirty / st->total);
	return 1;
}

/*
 * Given a local file "path" and the blocks created by a remote machine,
 * find out which blocks of our file they don't have and send them.
 * This function is reentrant: it must be called while there's still
 * data to send.
 * Return 0 if there's more data to send, >0 if the file has completed
 * its update, or <0 on error.
 */
int
blk_match(struct sess *sess, int fd, const struct blkset *blks,
	const char *path, struct blkstat *st)
{
	unsigned char	 filemd[MD4_DIGEST_LENGTH];
	int		 c;

	/*
	 * If the file's empty or we don't have any blocks from the
	 * sender, then simply send the whole file.
	 * Otherwise, run the hash matching routine and send raw chunks
	 * and subsequent matching tokens.
	 */

	if (st->mapsz && blks->blksz) {
		if ((c = blk_match_send(sess, path, fd, blks, st)) < 0) {
			ERRX1(sess, "blk_match_send");
			return -1;
		} else if (c == 0)
			return 0;
	} else {
		if (!blk_flush(sess, fd, st->map, st->mapsz, 0)) {
			ERRX1(sess, "blk_flush");
			return -1;
		}
		LOG3(sess, "%s: flushed (un-chunked) %jd B, 100%% upload ratio",
		    path, (intmax_t)st->mapsz);
	}

	/*
	 * Now write the full file hash.
	 * Since we're seeding the hash, this always gives us some sort
	 * of data even if the file's zero-length.
	 */

	hash_file(st->map, st->mapsz, filemd, sess);

	if (!io_write_buf(sess, fd, filemd, MD4_DIGEST_LENGTH)) {
		ERRX1(sess, "io_write_buf");
		return -1;
	}

	return 1;
}

/*
 * Sent from the sender to the receiver to indicate that the block set
 * has been received.
 * Symmetrises blk_send_ack().
 * Returns zero on failure, non-zero on success.
 */
int
blk_recv_ack(struct sess *sess,
	int fd, const struct blkset *blocks, int32_t idx)
{

	/* FIXME: put into static block. */

	if (!io_write_int(sess, fd, idx))
		ERRX1(sess, "io_write_int");
	else if (!io_write_int(sess, fd, blocks->blksz))
		ERRX1(sess, "io_write_int");
	else if (!io_write_int(sess, fd, blocks->len))
		ERRX1(sess, "io_write_int");
	else if (!io_write_int(sess, fd, blocks->csum))
		ERRX1(sess, "io_write_int");
	else if (!io_write_int(sess, fd, blocks->rem))
		ERRX1(sess, "io_write_int");
	else
		return 1;

	return 0;
}

/*
 * Read all of the checksums for a file's blocks.
 * Returns the set of blocks or NULL on failure.
 */
struct blkset *
blk_recv(struct sess *sess, int fd, const char *path)
{
	struct blkset	*s;
	int32_t		 i;
	size_t		 j;
	struct blk	*b;
	off_t		 offs = 0;

	if ((s = calloc(1, sizeof(struct blkset))) == NULL) {
		ERR(sess, "calloc");
		return NULL;
	}

	/*
	 * The block prologue consists of a few values that we'll need
	 * in reading the individual blocks for this file.
	 * FIXME: read into buffer and unbuffer.
	 */

	if (!io_read_size(sess, fd, &s->blksz)) {
		ERRX1(sess, "io_read_size");
		goto out;
	} else if (!io_read_size(sess, fd, &s->len)) {
		ERRX1(sess, "io_read_size");
		goto out;
	} else if (!io_read_size(sess, fd, &s->csum)) {
		ERRX1(sess, "io_read_int");
		goto out;
	} else if (!io_read_size(sess, fd, &s->rem)) {
		ERRX1(sess, "io_read_int");
		goto out;
	} else if (s->rem && s->rem >= s->len) {
		ERRX(sess, "block remainder is "
			"greater than block size");
		goto out;
	}

	LOG3(sess, "%s: read block prologue: %zu blocks of "
		"%zu B, %zu B remainder, %zu B checksum", path,
		s->blksz, s->len, s->rem, s->csum);

	if (s->blksz) {
		s->blks = calloc(s->blksz, sizeof(struct blk));
		if (s->blks == NULL) {
			ERR(sess, "calloc");
			goto out;
		}
	}

	/*
	 * Read each block individually.
	 * FIXME: read buffer and unbuffer.
	 */

	for (j = 0; j < s->blksz; j++) {
		b = &s->blks[j];
		if (!io_read_int(sess, fd, &i)) {
			ERRX1(sess, "io_read_int");
			goto out;
		}
		b->chksum_short = i;

		assert(s->csum <= sizeof(b->chksum_long));
		if (!io_read_buf(sess,
		    fd, b->chksum_long, s->csum)) {
			ERRX1(sess, "io_read_buf");
			goto out;
		}

		/*
		 * If we're the last block, then we're assigned the
		 * remainder of the data.
		 */

		b->offs = offs;
		b->idx = j;
		b->len = (j == (s->blksz - 1) && s->rem) ?
			s->rem : s->len;
		offs += b->len;

		LOG4(sess, "%s: read block %zu, "
			"length %zu B", path, b->idx, b->len);
	}

	s->size = offs;
	LOG3(sess, "%s: read blocks: %zu blocks, %jd B total "
		"blocked data", path, s->blksz, (intmax_t)s->size);
	return s;
out:
	free(s->blks);
	free(s);
	return NULL;
}

/*
 * Symmetrise blk_recv_ack(), except w/o the leading identifier.
 * Return zero on failure, non-zero on success.
 */
int
blk_send_ack(struct sess *sess, int fd, struct blkset *p)
{
	char	 buf[16];
	size_t	 pos = 0, sz;

	/* Put the entire send routine into a buffer. */

	sz = sizeof(int32_t) + /* block count */
	     sizeof(int32_t) + /* block length */
	     sizeof(int32_t) + /* checksum length */
	     sizeof(int32_t); /* block remainder */
	assert(sz <= sizeof(buf));

	if (!io_read_buf(sess, fd, buf, sz)) {
		ERRX1(sess, "io_read_buf");
		return 0;
	}

	if (!io_unbuffer_size(sess, buf, &pos, sz, &p->blksz))
		ERRX1(sess, "io_unbuffer_size");
	else if (!io_unbuffer_size(sess, buf, &pos, sz, &p->len))
		ERRX1(sess, "io_unbuffer_size");
	else if (!io_unbuffer_size(sess, buf, &pos, sz, &p->csum))
		ERRX1(sess, "io_unbuffer_size");
	else if (!io_unbuffer_size(sess, buf, &pos, sz, &p->rem))
		ERRX1(sess, "io_unbuffer_size");
	else if (p->len && p->rem >= p->len)
		ERRX1(sess, "non-zero length is less than remainder");
	else if (p->csum == 0 || p->csum > 16)
		ERRX1(sess, "inappropriate checksum length");
	else
		return 1;

	return 0;
}

/*
 * Transmit the metadata for set and blocks.
 * Return zero on failure, non-zero on success.
 */
int
blk_send(struct sess *sess, int fd, size_t idx,
	const struct blkset *p, const char *path)
{
	char	*buf;
	size_t	 i, pos = 0, sz;
	int	 rc = 0;

	/* Put the entire send routine into a buffer. */

	sz = sizeof(int32_t) + /* identifier */
	    sizeof(int32_t) + /* block count */
	    sizeof(int32_t) + /* block length */
	    sizeof(int32_t) + /* checksum length */
	    sizeof(int32_t) + /* block remainder */
	    p->blksz *
	    (sizeof(int32_t) + /* short checksum */
		p->csum); /* long checksum */

	if ((buf = malloc(sz)) == NULL) {
		ERR(sess, "malloc");
		return 0;
	}

	io_buffer_int(sess, buf, &pos, sz, idx);
	io_buffer_int(sess, buf, &pos, sz, p->blksz);
	io_buffer_int(sess, buf, &pos, sz, p->len);
	io_buffer_int(sess, buf, &pos, sz, p->csum);
	io_buffer_int(sess, buf, &pos, sz, p->rem);

	for (i = 0; i < p->blksz; i++) {
		io_buffer_int(sess, buf, &pos,
			sz, p->blks[i].chksum_short);
		io_buffer_buf(sess, buf, &pos, sz,
			p->blks[i].chksum_long, p->csum);
	}

	assert(pos == sz);

	if (!io_write_buf(sess, fd, buf, sz)) {
		ERRX1(sess, "io_write_buf");
		goto out;
	}

	LOG3(sess, "%s: sent block prologue: %zu blocks of %zu B, "
		"%zu B remainder, %zu B checksum", path,
		p->blksz, p->len, p->rem, p->csum);
	rc = 1;
out:
	free(buf);
	return rc;
}
