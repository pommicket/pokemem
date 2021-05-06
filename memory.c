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
