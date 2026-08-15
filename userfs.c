#include "userfs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	char *memory;
	int occupied;
	struct block *next;
	struct block *prev;
};

struct file {
	struct block *block_list;
	struct block *last_block;
	int refs;
	char *name;
	struct file *next;
	struct file *prev;
	
	size_t size;
	int is_deleted;
};

static struct file *file_list = NULL;

struct filedesc {
	struct file *file;
	struct block *current_block;
	int offset_in_block;
	size_t position;
#if NEED_OPEN_FLAGS
	int flags;
#endif
};

static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

enum ufs_error_code ufs_errno() {
	return ufs_error_code;
}

static void
update_desc_position(struct filedesc *desc)
{
	struct file *f = desc->file;
	size_t pos = 0;
	struct block *b = f->block_list;
	
	while (b) {
		if (pos + b->occupied > desc->position) {
			desc->current_block = b;
			desc->offset_in_block = desc->position - pos;
			return;
		}
		pos += b->occupied;
		b = b->next;
	}
	
	desc->current_block = f->last_block;
	desc->offset_in_block = f->last_block ? f->last_block->occupied : 0;
	
	if (desc->current_block && desc->offset_in_block == BLOCK_SIZE) {
		desc->current_block = NULL;
		desc->offset_in_block = 0;
	}
}

int
ufs_open(const char *filename, int flags)
{
	struct file *f = file_list;
	while (f) {
		if (!f->is_deleted && strcmp(f->name, filename) == 0) {
			break;
		}
		f = f->next;
	}

	if (!f) {
		if (!(flags & UFS_CREATE)) {
			ufs_error_code = UFS_ERR_NO_FILE;
			return -1;
		}
		f = (struct file *)malloc(sizeof(struct file));
		f->name = (char *)malloc(strlen(filename) + 1);
		strcpy(f->name, filename);
		f->block_list = NULL;
		f->last_block = NULL;
		f->refs = 0;
		f->next = file_list;
		f->prev = NULL;
		if (file_list) file_list->prev = f;
		file_list = f;
		f->size = 0;
		f->is_deleted = 0;
	}

	f->refs++;

	int fd = -1;
	for (int i = 0; i < file_descriptor_count; ++i) {
		if (!file_descriptors[i]) {
			fd = i;
			break;
		}
	}
	if (fd == -1) {
		if (file_descriptor_count == file_descriptor_capacity) {
			int new_cap = file_descriptor_capacity ? file_descriptor_capacity * 2 : 16;
			file_descriptors = (struct filedesc **)realloc(file_descriptors, new_cap * sizeof(struct filedesc *));
			file_descriptor_capacity = new_cap;
		}
		fd = file_descriptor_count++;
	}

	struct filedesc *desc = (struct filedesc *)malloc(sizeof(struct filedesc));
	desc->file = f;
	desc->position = 0;
	update_desc_position(desc);

#if NEED_OPEN_FLAGS
	if (flags & UFS_READ_ONLY) desc->flags = UFS_READ_ONLY;
	else if (flags & UFS_WRITE_ONLY) desc->flags = UFS_WRITE_ONLY;
	else desc->flags = UFS_READ_WRITE;
#endif

	file_descriptors[fd] = desc;
	ufs_error_code = UFS_ERR_NO_ERR;
	return fd;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	if (fd < 0 || fd >= file_descriptor_count || !file_descriptors[fd]) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	struct filedesc *desc = file_descriptors[fd];
	struct file *f = desc->file;

#if NEED_OPEN_FLAGS
	if (desc->flags == UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if (size == 0) return 0;

	size_t end_pos = desc->position + size;
	if (end_pos > f->size && end_pos > MAX_FILE_SIZE) {
		size = MAX_FILE_SIZE - desc->position;
		if (size == 0) {
			ufs_error_code = UFS_ERR_NO_MEM;
			return -1;
		}
	}

	/* ИСПРАВЛЕНИЕ: Если блок потерялся (например, файл был пуст при открытии),
	   пересчитываем его на основе текущей позиции */
	if (!desc->current_block) {
		update_desc_position(desc);
	}

	size_t written = 0;
	while (written < size) {
		if (desc->current_block && desc->offset_in_block == BLOCK_SIZE) {
			desc->current_block = desc->current_block->next;
			desc->offset_in_block = 0;
		}
		if (!desc->current_block) {
			struct block *b = (struct block *)malloc(sizeof(struct block));
			b->memory = (char *)malloc(BLOCK_SIZE);
			b->occupied = 0;
			b->next = NULL;
			b->prev = f->last_block;
			if (f->last_block) f->last_block->next = b;
			else f->block_list = b;
			f->last_block = b;
			desc->current_block = b;
			desc->offset_in_block = 0;
		}

		int space_in_block = BLOCK_SIZE - desc->offset_in_block;
		size_t to_write = size - written;
		if (to_write > (size_t)space_in_block) to_write = space_in_block;

		memcpy(desc->current_block->memory + desc->offset_in_block, buf + written, to_write);

		desc->offset_in_block += to_write;
		if (desc->current_block->occupied < desc->offset_in_block) {
			desc->current_block->occupied = desc->offset_in_block;
		}

		size_t new_end = desc->position + to_write;
		if (new_end > f->size) f->size = new_end;

		desc->position += to_write;
		written += to_write;

		if (desc->offset_in_block == BLOCK_SIZE) {
			desc->current_block = desc->current_block->next;
			desc->offset_in_block = 0;
		}
	}
	
	ufs_error_code = UFS_ERR_NO_ERR;
	return written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	if (fd < 0 || fd >= file_descriptor_count || !file_descriptors[fd]) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	struct filedesc *desc = file_descriptors[fd];
	struct file *f = desc->file;

#if NEED_OPEN_FLAGS
	if (desc->flags == UFS_WRITE_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if (size == 0) return 0;
	if (desc->position >= f->size) return 0; /* EOF */

	/* ИСПРАВЛЕНИЕ: Аналогично для чтения */
	if (!desc->current_block) {
		update_desc_position(desc);
	}

	size_t remaining = f->size - desc->position;
	if (size > remaining) size = remaining;

	size_t read_bytes = 0;
	while (read_bytes < size) {
		if (desc->current_block && desc->offset_in_block == desc->current_block->occupied) {
			desc->current_block = desc->current_block->next;
			desc->offset_in_block = 0;
		}
		if (!desc->current_block) break;

		int available = desc->current_block->occupied - desc->offset_in_block;
		size_t to_read = size - read_bytes;
		if (to_read > (size_t)available) to_read = available;

		memcpy(buf + read_bytes, desc->current_block->memory + desc->offset_in_block, to_read);

		desc->offset_in_block += to_read;
		desc->position += to_read;
		read_bytes += to_read;

		if (desc->offset_in_block == desc->current_block->occupied) {
			desc->current_block = desc->current_block->next;
			desc->offset_in_block = 0;
		}
	}

	ufs_error_code = UFS_ERR_NO_ERR;
	return read_bytes;
}

int
ufs_close(int fd)
{
	if (fd < 0 || fd >= file_descriptor_count || !file_descriptors[fd]) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	struct filedesc *desc = file_descriptors[fd];
	struct file *f = desc->file;

	f->refs--;
	
	if (f->refs == 0 && f->is_deleted) {
		if (f->prev) f->prev->next = f->next;
		else file_list = f->next;
		if (f->next) f->next->prev = f->prev;

		struct block *b = f->block_list;
		while (b) {
			struct block *next = b->next;
			free(b->memory);
			free(b);
			b = next;
		}
		free(f->name);
		free(f);
	}

	free(desc);
	file_descriptors[fd] = NULL;
	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
}

int
ufs_delete(const char *filename)
{
	struct file *f = file_list;
	while (f) {
		if (!f->is_deleted && strcmp(f->name, filename) == 0) break;
		f = f->next;
	}
	if (!f) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	f->is_deleted = 1;

	if (f->refs == 0) {
		if (f->prev) f->prev->next = f->next;
		else file_list = f->next;
		if (f->next) f->next->prev = f->prev;

		struct block *b = f->block_list;
		while (b) {
			struct block *next = b->next;
			free(b->memory);
			free(b);
			b = next;
		}
		free(f->name);
		free(f);
	}

	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
	if (fd < 0 || fd >= file_descriptor_count || !file_descriptors[fd]) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	struct filedesc *desc = file_descriptors[fd];
	struct file *f = desc->file;

#if NEED_OPEN_FLAGS
	if (desc->flags == UFS_READ_ONLY) {
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	if (new_size < f->size) {
		size_t current_pos = 0;
		struct block *b = f->block_list;
		while (b) {
			if (current_pos + b->occupied > new_size) {
				int new_occupied = new_size - current_pos;
				if (new_occupied == 0 && current_pos == 0) {
					struct block *next = b->next;
					while (next) {
						struct block *to_free = next;
						next = next->next;
						free(to_free->memory);
						free(to_free);
					}
					free(b->memory);
					free(b);
					f->block_list = NULL;
					f->last_block = NULL;
				} else {
					b->occupied = new_occupied;
					struct block *next = b->next;
					b->next = NULL;
					f->last_block = b;
					while (next) {
						struct block *to_free = next;
						next = next->next;
						free(to_free->memory);
						free(to_free);
					}
				}
				break;
			}
			current_pos += b->occupied;
			b = b->next;
		}
		f->size = new_size;

		for (int i = 0; i < file_descriptor_count; ++i) {
			struct filedesc *d = file_descriptors[i];
			if (d && d->file == f) {
				if (d->position > new_size) d->position = new_size;
				update_desc_position(d);
			}
		}
	} else if (new_size > f->size) {
		size_t to_add = new_size - f->size;
		while (to_add > 0) {
			struct block *b = NULL;
			if (f->last_block && f->last_block->occupied < BLOCK_SIZE) {
				b = f->last_block;
			} else {
				b = (struct block *)malloc(sizeof(struct block));
				b->memory = (char *)malloc(BLOCK_SIZE);
				memset(b->memory, 0, BLOCK_SIZE);
				b->occupied = 0;
				b->next = NULL;
				b->prev = f->last_block;
				if (f->last_block) f->last_block->next = b;
				else f->block_list = b;
				f->last_block = b;
			}

			size_t space = BLOCK_SIZE - b->occupied;
			size_t add = to_add < space ? to_add : space;
			b->occupied += add;
			to_add -= add;
		}
		f->size = new_size;
	}

	ufs_error_code = UFS_ERR_NO_ERR;
	return 0;
}

#endif

void
ufs_destroy(void)
{
	struct file *f = file_list;
	while (f) {
		struct file *next = f->next;
		struct block *b = f->block_list;
		while (b) {
			struct block *b_next = b->next;
			free(b->memory);
			free(b);
			b = b_next;
		}
		free(f->name);
		free(f);
		f = next;
	}
	file_list = NULL;

	for (int i = 0; i < file_descriptor_count; ++i) {
		if (file_descriptors[i]) {
			free(file_descriptors[i]);
			file_descriptors[i] = NULL;
		}
	}
	free(file_descriptors);
	file_descriptors = NULL;
	file_descriptor_count = 0;
	file_descriptor_capacity = 0;
}