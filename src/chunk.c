/**
 * the network chunk-API
 *
 *
 */

#include "chunk.h"
#include "base.h"
#include "log.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>

chunkqueue *chunkqueue_init(void) {
	chunkqueue *cq;

	cq = calloc(1, sizeof(*cq));

	cq->first = NULL;
	cq->last = NULL;

	cq->unused = NULL;

	return cq;
}

static chunk *chunk_init(void) {
	chunk *c;

	c = calloc(1, sizeof(*c));

	c->type = MEM_CHUNK;
	c->mem = buffer_init();
	c->file.name = buffer_init();
	c->file.start = c->file.length = c->file.mmap.offset = 0;
	c->file.fd = -1;
	c->file.mmap.start = MAP_FAILED;
	c->file.mmap.length = 0;
	c->file.is_temp = 0;
	c->offset = 0;
	c->next = NULL;

	return c;
}

static void chunk_reset(chunk *c) {
	if (NULL == c) return;

	c->type = MEM_CHUNK;

	buffer_reset(c->mem);

	if (c->file.is_temp && !buffer_string_is_empty(c->file.name)) {
		unlink(c->file.name->ptr);
	}

	buffer_reset(c->file.name);

	if (c->file.fd != -1) {
		close(c->file.fd);
		c->file.fd = -1;
	}
	if (MAP_FAILED != c->file.mmap.start) {
		munmap(c->file.mmap.start, c->file.mmap.length);
		c->file.mmap.start = MAP_FAILED;
	}
	c->file.start = c->file.length = c->file.mmap.offset = 0;
	c->file.mmap.length = 0;
	c->file.is_temp = 0;
	c->offset = 0;
	c->next = NULL;
}

static void chunk_free(chunk *c) {
	if (NULL == c) return;

	chunk_reset(c);

	buffer_free(c->mem);
	buffer_free(c->file.name);

	free(c);
}

void chunkqueue_free(chunkqueue *cq) {
	chunk *c, *pc;

	if (NULL == cq) return;

	for (c = cq->first; c; ) {
		pc = c;
		c = c->next;
		chunk_free(pc);
	}

	for (c = cq->unused; c; ) {
		pc = c;
		c = c->next;
		chunk_free(pc);
	}

	free(cq);
}

static void chunkqueue_push_unused_chunk(chunkqueue *cq, chunk *c) {
	force_assert(NULL != cq && NULL != c);

	/* keep at max 4 chunks in the 'unused'-cache */
	if (cq->unused_chunks > 4) {
		chunk_free(c);
	} else {
		chunk_reset(c);
		c->next = cq->unused;
		cq->unused = c;
		cq->unused_chunks++;
	}
}

static chunk *chunkqueue_get_unused_chunk(chunkqueue *cq) {
	chunk *c;

	force_assert(NULL != cq);

	/* check if we have a unused chunk */
	if (0 == cq->unused) {
		c = chunk_init();
	} else {
		/* take the first element from the list (a stack) */
		c = cq->unused;
		cq->unused = c->next;
		c->next = NULL;
		cq->unused_chunks--;
	}

	return c;
}

static void chunkqueue_prepend_chunk(chunkqueue *cq, chunk *c) {
	c->next = cq->first;
	cq->first = c;

	if (NULL == cq->last) {
		cq->last = c;
	}
}

static void chunkqueue_append_chunk(chunkqueue *cq, chunk *c) {
	if (cq->last) {
		cq->last->next = c;
	}
	cq->last = c;

	if (NULL == cq->first) {
		cq->first = c;
	}
}

void chunkqueue_reset(chunkqueue *cq) {
	chunk *cur = cq->first;

	cq->first = cq->last = NULL;

	while (NULL != cur) {
		chunk *next = cur->next;
		chunkqueue_push_unused_chunk(cq, cur);
		cur = next;
	}

	cq->bytes_in = 0;
	cq->bytes_out = 0;
}

void chunkqueue_append_file(chunkqueue *cq, buffer *fn, off_t offset, off_t len) {
	chunk *c;

	if (0 == len) return;

	c = chunkqueue_get_unused_chunk(cq);

	c->type = FILE_CHUNK;

	buffer_copy_buffer(c->file.name, fn);
	c->file.start = offset;
	c->file.length = len;
	c->offset = 0;

	chunkqueue_append_chunk(cq, c);
}

void chunkqueue_append_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;

	if (buffer_string_is_empty(mem)) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	force_assert(NULL != c->mem);
	buffer_move(c->mem, mem);

	chunkqueue_append_chunk(cq, c);
}

void chunkqueue_prepend_buffer(chunkqueue *cq, buffer *mem) {
	chunk *c;

	if (buffer_string_is_empty(mem)) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	force_assert(NULL != c->mem);
	buffer_move(c->mem, mem);

	chunkqueue_prepend_chunk(cq, c);
}


void chunkqueue_append_mem(chunkqueue *cq, const char * mem, size_t len) {
	chunk *c;

	if (0 == len) return;

	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	buffer_copy_string_len(c->mem, mem, len);

	chunkqueue_append_chunk(cq, c);
}

void chunkqueue_get_memory(chunkqueue *cq, char **mem, size_t *len, size_t min_size, size_t alloc_size) {
	static const size_t REALLOC_MAX_SIZE = 256;
	chunk *c;
	buffer *b;
	char *dummy_mem;
	size_t dummy_len;

	force_assert(NULL != cq);
	if (NULL == mem) mem = &dummy_mem;
	if (NULL == len) len = &dummy_len;

	/* default values: */
	if (0 == min_size) min_size = 1024;
	if (0 == alloc_size) alloc_size = 4096;
	if (alloc_size < min_size) alloc_size = min_size;

	if (NULL != cq->last && MEM_CHUNK == cq->last->type) {
		size_t have;

		b = cq->last->mem;
		have = buffer_string_space(b);

		/* unused buffer: allocate space */
		if (buffer_string_is_empty(b)) {
			buffer_string_prepare_copy(b, alloc_size);
			have = buffer_string_space(b);
		}
		/* if buffer is really small just make it bigger */
		else if (have < min_size && b->size <= REALLOC_MAX_SIZE) {
			size_t cur_len = buffer_string_length(b);
			size_t new_size = cur_len + min_size, append;
			if (new_size < alloc_size) new_size = alloc_size;

			append = new_size - cur_len;
			if (append >= min_size) {
				buffer_string_prepare_append(b, append);
				have = buffer_string_space(b);
			}
		}

		/* return pointer into existing buffer if large enough */
		if (have >= min_size) {
			*mem = b->ptr + buffer_string_length(b);
			*len = have;
			return;
		}
	}

	/* allocate new chunk */
	c = chunkqueue_get_unused_chunk(cq);
	c->type = MEM_CHUNK;
	chunkqueue_append_chunk(cq, c);

	b = c->mem;
	buffer_string_prepare_append(b, alloc_size);

	*mem = b->ptr + buffer_string_length(b);
	*len = buffer_string_space(b);
}

void chunkqueue_use_memory(chunkqueue *cq, size_t len) {
	buffer *b;

	force_assert(NULL != cq);
	force_assert(NULL != cq->last && MEM_CHUNK == cq->last->type);
	b = cq->last->mem;

	if (len > 0) {
		buffer_commit(b, len);
	} else if (buffer_string_is_empty(b)) {
		/* unused buffer: can't remove chunk easily from
		 * end of list, so just reset the buffer
		 */
		buffer_reset(b);
	}
}

void chunkqueue_set_tempdirs(chunkqueue *cq, array *tempdirs) {
	force_assert(NULL != cq);
	cq->tempdirs = tempdirs;
}

void chunkqueue_steal(chunkqueue *dest, chunkqueue *src, off_t len) {
	while (len > 0) {
		chunk *c = src->first;
		off_t clen = 0, use;

		if (NULL == c) break;

		switch (c->type) {
		case MEM_CHUNK:
			clen = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			clen = c->file.length;
			break;
		}
		force_assert(clen >= c->offset);
		clen -= c->offset;
		use = len >= clen ? clen : len;

		src->bytes_out += use;
		dest->bytes_in += use;
		len -= use;

		if (0 == clen) {
			/* drop empty chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
			continue;
		}

		if (use == clen) {
			/* move complete chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;

			chunkqueue_append_chunk(dest, c);
			continue;
		}

		/* partial chunk with length "use" */

		switch (c->type) {
		case MEM_CHUNK:
			chunkqueue_append_mem(dest, c->mem->ptr + c->offset, use);
			break;
		case FILE_CHUNK:
			/* tempfile flag is in "last" chunk after the split */
			chunkqueue_append_file(dest, c->file.name, c->file.start + c->offset, use);
			break;
		}

		c->offset += use;
		force_assert(0 == len);
	}
}

static chunk *chunkqueue_get_append_tempfile(chunkqueue *cq) {
	chunk *c;
	buffer *template = buffer_init_string("/var/tmp/lighttpd-upload-XXXXXX");
	int fd;

	if (cq->tempdirs && cq->tempdirs->used) {
		size_t i;

		/* we have several tempdirs, only if all of them fail we jump out */

		for (i = 0; i < cq->tempdirs->used; i++) {
			data_string *ds = (data_string *)cq->tempdirs->data[i];

			buffer_copy_buffer(template, ds->value);
			buffer_append_slash(template);
			buffer_append_string_len(template, CONST_STR_LEN("lighttpd-upload-XXXXXX"));

			if (-1 != (fd = mkstemp(template->ptr))) break;
		}
	} else {
		fd = mkstemp(template->ptr);
	}

	if (-1 == fd) {
		buffer_free(template);
		return NULL;
	}

	c = chunkqueue_get_unused_chunk(cq);
	c->type = FILE_CHUNK;
	c->file.fd = fd;
	c->file.is_temp = 1;
	buffer_copy_buffer(c->file.name, template);
	c->file.length = 0;

	chunkqueue_append_chunk(cq, c);

	buffer_free(template);

	return c;
}

static int chunkqueue_append_to_tempfile(server *srv, chunkqueue *dest, const char *mem, size_t len) {
	chunk *dst_c = NULL;
	ssize_t written;
	/* copy everything to max 1Mb sized tempfiles */

	/*
	 * if the last chunk is
	 * - smaller than 1Mb (size < 1Mb)
	 * - not read yet (offset == 0)
	 * -> append to it
	 * otherwise
	 * -> create a new chunk
	 *
	 * */

	if (NULL != dest->last
		&& FILE_CHUNK != dest->last->type
		&& dest->last->file.is_temp
		&& -1 != dest->last->file.fd
		&& 0 == dest->last->offset) {
		/* ok, take the last chunk for our job */
		dst_c = dest->last;

		if (dest->last->file.length >= 1 * 1024 * 1024) {
			/* the chunk is too large now, close it */
			if (-1 != dst_c->file.fd) {
				close(dst_c->file.fd);
				dst_c->file.fd = -1;
			}
			dst_c = chunkqueue_get_append_tempfile(dest);
		}
	} else {
		dst_c = chunkqueue_get_append_tempfile(dest);
	}

	if (NULL == dst_c) {
		/* we don't have file to write to,
		 * EACCES might be one reason.
		 *
		 * Instead of sending 500 we send 413 and say the request is too large
		 */

		log_error_write(srv, __FILE__, __LINE__, "ss",
			"denying upload as opening temp-file for upload failed:",
			strerror(errno));

		return -1;
	}

	if (0 > (written = write(dst_c->file.fd, mem, len)) || (size_t) written != len) {
		/* write failed for some reason ... disk full ? */
		log_error_write(srv, __FILE__, __LINE__, "sbs",
				"denying upload as writing to file failed:",
				dst_c->file.name, strerror(errno));

		close(dst_c->file.fd);
		dst_c->file.fd = -1;

		return -1;
	}

	dst_c->file.length += len;

	return 0;
}

int chunkqueue_steal_with_tempfiles(server *srv, chunkqueue *dest, chunkqueue *src, off_t len) {
	while (len > 0) {
		chunk *c = src->first;
		off_t clen = 0, use;

		if (NULL == c) break;

		switch (c->type) {
		case MEM_CHUNK:
			clen = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			clen = c->file.length;
			break;
		}
		force_assert(clen >= c->offset);
		clen -= c->offset;
		use = len >= clen ? clen : len;

		src->bytes_out += use;
		dest->bytes_in += use;
		len -= use;

		if (0 == clen) {
			/* drop empty chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
			continue;
		}

		if (FILE_CHUNK == c->type) {
			if (use == clen) {
				/* move complete chunk */
				src->first = c->next;
				if (c == src->last) src->last = NULL;

				chunkqueue_append_chunk(dest, c);
			} else {
				/* partial chunk with length "use" */
				/* tempfile flag is in "last" chunk after the split */
				chunkqueue_append_file(dest, c->file.name, c->file.start + c->offset, use);

				c->offset += use;
				force_assert(0 == len);
			}
			continue;
		}

		/* store "use" bytes from memory chunk in tempfile */
		if (0 != chunkqueue_append_to_tempfile(srv, dest, c->mem->ptr + c->offset, use)) {
			/* undo counters */
			src->bytes_out -= use;
			dest->bytes_in -= use;
			return -1;
		}


		c->offset += use;
		if (use == clen) {
			/* finished chunk */
			src->first = c->next;
			if (c == src->last) src->last = NULL;
			chunkqueue_push_unused_chunk(src, c);
		}
	}

	return 0;
}

off_t chunkqueue_length(chunkqueue *cq) {
	off_t len = 0;
	chunk *c;

	for (c = cq->first; c; c = c->next) {
		off_t c_len = 0;

		switch (c->type) {
		case MEM_CHUNK:
			c_len = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			c_len = c->file.length;
			break;
		}
		force_assert(c_len >= c->offset);
		len += c_len - c->offset;
	}

	return len;
}

int chunkqueue_is_empty(chunkqueue *cq) {
	return NULL == cq->first;
}

void chunkqueue_remove_finished_chunks(chunkqueue *cq) {
	chunk *c;

	for (c = cq->first; c; c = cq->first) {
		off_t c_len = 0;

		switch (c->type) {
		case MEM_CHUNK:
			c_len = buffer_string_length(c->mem);
			break;
		case FILE_CHUNK:
			c_len = c->file.length;
			break;
		}
		force_assert(c_len >= c->offset);

		if (c_len > c->offset) break; /* not finished yet */

		cq->first = c->next;
		if (c == cq->last) cq->last = NULL;

		chunkqueue_push_unused_chunk(cq, c);
	}
}
