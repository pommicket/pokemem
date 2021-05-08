// low-level stuff for reading/writing the memory of another process

static void close_process(State *state, char const *reason) {
	GtkBuilder *builder = state->builder;
	free(state->maps); state->maps = NULL;
	state->nmaps = 0;
	state->pid = 0;
	{
		GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
		gtk_list_store_clear(store);
	}
	if (reason)
		display_info(state, "Can't access process anymore: %s", reason);
	else
		display_info_nofmt(state, "Can't access process anymore.");
}

// don't use this function; use one of the ones below
static int memory_open(State *state, int flags) {
	if (state->pid) {
		if (state->stop_while_accessing_memory) {
			if (kill(state->pid, SIGSTOP) == -1) {
				close_process(state, strerror(errno));
				return 0;
			}
		}
		char name[64];
		sprintf(name, "/proc/%lld/mem", (long long)state->pid);
		int fd = open(name, flags);
		if (fd == -1) {
			close_process(state, strerror(errno));
			return 0;
		}
		return fd;
	}
	return 0;
}

static void memory_close(State *state, int fd) {
	if (state->stop_while_accessing_memory) {
		kill(state->pid, SIGCONT);
	}
	if (fd) close(fd);
}

	
// get a file descriptor for reading memory from the process
// returns 0 on failure
static int memory_reader_open(State *state) {
	return memory_open(state, O_RDONLY);
}

static void memory_reader_close(State *state, int reader) {
	memory_close(state, reader);
}

// like memory_reader_open, but for writing to memory
static int memory_writer_open(State *state) {
	return memory_open(state, O_WRONLY);
}

static void memory_writer_close(State *state, int writer) {
	memory_close(state, writer);
}

static uint8_t memory_read_byte(int reader, Address addr) {
	lseek(reader, (off_t)addr, SEEK_SET);
	uint8_t byte = 0;
	read(reader, &byte, 1);
	return byte;
}

// returns number of bytes successfully read
static Address memory_read_bytes(int reader, Address addr, uint8_t *memory, Address nbytes) {
	lseek(reader, (off_t)addr, SEEK_SET);
	Address idx = 0;
	while (idx < nbytes) {
		ssize_t n = read(reader, &memory[idx], (size_t)(nbytes - idx));
		if (n <= 0) { break; }
		idx += (Address)n;
	}
	return idx;
}

// returns # of bytes written (so either 0 or 1)
static Address memory_write_byte(int writer, Address addr, uint8_t byte) {
	lseek(writer, (off_t)addr, SEEK_SET);
	if (write(writer, &byte, 1) == 1)
		return 1;
	else
		return 0;
}

// returns # of bytes written
static Address memory_write_bytes(int writer, Address addr, uint8_t const *bytes, Address nbytes) {
	lseek(writer, (off_t)addr, SEEK_SET);
	Address idx = 0;
	while (idx < nbytes) {
		ssize_t n = write(writer, &bytes[idx], (size_t)(nbytes - idx));
		if (n < 0) break;
		idx += (Address)n;
	}
	return idx;
}

typedef struct {
	FILE *fp;
	Address curr_addr;
} MemfileWriter;
static char const MEMFILE_IDENT[4] = {'\xff', 'M', 'E', 'M'};

static bool memfile_writer_open(State *state, MemfileWriter *writer, char const *filename) {
	memset(writer, 0, sizeof *writer);
	writer->fp = fopen(filename, "wb");
	if (writer->fp) {
		fwrite(MEMFILE_IDENT, 1, sizeof MEMFILE_IDENT, writer->fp);
		return true;
	} else {
		display_error(state, "Couldn't open %s: %s.", filename, strerror(errno));
		return false;
	}
}

static void memfile_writer_close(MemfileWriter *writer) {
	if (writer->fp) fclose(writer->fp);
	memset(writer, 0, sizeof *writer);
}


// write up to 64K of consecutive memory.
static void memfile_write_bytes(MemfileWriter *writer, Address addr, uint8_t const *data, size_t nbytes) {
	Address addr_increment = addr - writer->curr_addr;
	// set address
	if (addr_increment < 64) {
		putc((int)addr_increment, writer->fp);
	} else if (addr_increment < 8192) {
		putc(0x40 | (int)(addr_increment & 0x1f), writer->fp);
		putc((int)(addr_increment >> 5), writer->fp);
	} else {
		putc(0x60, writer->fp);
		fwrite(&addr, sizeof addr, 1, writer->fp);
	}
	
	if (nbytes < 64) {
		putc(0x80 | (int)nbytes, writer->fp);
	} else if (nbytes < 65536) {
		putc(0xC0, writer->fp);
		fwrite(&nbytes, 2, 1, writer->fp);
	} else {
		putc(0xE0, writer->fp);
		fwrite(&nbytes, sizeof nbytes, 1, writer->fp);
	}
	fwrite(data, 1, nbytes, writer->fp);
	
	writer->curr_addr = addr + nbytes;
}

static void memfile_write_byte(MemfileWriter *writer, Address addr, uint8_t byte) {
	memfile_write_bytes(writer, addr, &byte, 1);
}

static void memfile_write_all(State *state, char const *filename) {
	if (!state->pid) return;
	MemfileWriter writer = {0};
	if (memfile_writer_open(state, &writer, filename)) {
		int reader = memory_reader_open(state);
		if (reader) {
			for (unsigned m = 0; m < state->nmaps; ++m) {
				Map *map = &state->maps[m];
				uint8_t chunk[4096] = {0}; // page size is probably a multiple of 4096.
				for (Address offset = 0; offset < map->size; offset += sizeof chunk) {
					Address addr = map->lo + offset;
					memory_read_bytes(reader, addr, chunk, sizeof chunk);
					memfile_write_bytes(&writer, addr, chunk, sizeof chunk);
				}
			}
			memory_reader_close(state, reader);
		}
		memfile_writer_close(&writer);
	}
}

static void memfile_write_candidates(State *state, char const *filename) {
	if (!state->pid) return;
	size_t item_size = data_type_size(state->data_type);
	MemfileWriter writer = {0};
	if (memfile_writer_open(state, &writer, filename)) {
		int reader = memory_reader_open(state);
		if (reader) {
			Address bitset_index = 0;
			uint64_t *search_candidates = state->search_candidates;
			for (unsigned m = 0; m < state->nmaps; ++m) {
				Map *map = &state->maps[m];
				Address n_items = map->size / item_size;
				for (Address i = 0; i < n_items; ) {
					if (bitset_index % 64 == 0 && search_candidates[bitset_index / 64] == 0) {
						// no candidates here
						i += 64;
						bitset_index += 64;
					} else {
						if (search_candidates[bitset_index / 64] & MASK64(bitset_index % 64)) {
							// a candidate!
							Address addr = map->lo + i * item_size;
							uint64_t value = 0;
							memory_read_bytes(reader, addr, (uint8_t *)&value, item_size);
							memfile_write_bytes(&writer, addr, (uint8_t const *)&value, item_size);
						}
						++i;
						++bitset_index;
					}
				}
			}
			memory_reader_close(state, reader);
		}
		memfile_writer_close(&writer);
	}
}

static void memfile_load(State *state, char const *filename) {
	if (!state->pid) return;
	FILE *fp = fopen(filename, "rb");
	if (fp) {
		char ident[sizeof MEMFILE_IDENT] = {0};
		fread(ident, sizeof ident, 1, fp);
		if (memcmp(ident, MEMFILE_IDENT, sizeof MEMFILE_IDENT) != 0) {
			display_error(state, "%s is not a memory file.", filename);
		} else {
			int writer = memory_writer_open(state);
			if (writer) {
				Address addr = 0;
				
				int first_byte;
				while ((first_byte = getc(fp)) != EOF) {
					uint64_t val = 0;
					size_t nbytes = 0;
					switch (first_byte & 0xE0) {
					case 0x00:
					case 0x20:
						// 6 bits address increment
						addr += first_byte & 0x3F;
						break;
					case 0x40:
						// 5 bits + 1 byte address increment
						addr += first_byte & 0x1F;
						addr += (unsigned)getc(fp) << 5;
						break;
					case 0x60:
						// constant 4/8-byte address
						if (first_byte != 0x60)
							goto invalid;
						fread(&val, sizeof(Address), 1, fp);
						addr = val;
						break;
					case 0x80:
					case 0xA0:
						// 6 bit length; data
						nbytes = first_byte & 0x3F;
						goto read_data;
					case 0xC0:
						if (first_byte != 0xC0) goto invalid;
						// 2 bytes length; data
						fread(&nbytes, 2, 1, fp);
						goto read_data;
					case 0xE0:
						if (first_byte != 0xE0) goto invalid;
						// 4/8 bytes length; data
						fread(&nbytes, sizeof(size_t), 1, fp);
						goto read_data;
					read_data: {
						uint8_t chunk[4096] = {0};
						size_t bytes_left = nbytes;
						while (bytes_left > 0) {
							size_t chunk_len = bytes_left;
							if (chunk_len > sizeof chunk) chunk_len = sizeof chunk;
							fread(chunk, 1, chunk_len, fp);
							memory_write_bytes(writer, addr, chunk, chunk_len);
							addr += chunk_len;
							bytes_left -= chunk_len;
						}
					} break;
					invalid:
						display_error(state, "%s is an invalid memory file.", filename);
						goto eof;
					}
				}
			eof:
				memory_writer_close(state, writer);
			}
		}
		fclose(fp);
	} else {
		display_error(state, "Couldn't open %s: %s.", filename, strerror(errno));
	}
}
