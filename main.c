#include <gtk/gtk.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>

typedef pid_t PID;
typedef uint64_t Address;
#define SCNxADDR SCNx64
#define PRIdADDR PRId64
#define PRIxADDR PRIx64

// a memory map
typedef struct {
	Address lo, size;
} Map;

typedef struct {
	GtkWindow *window;
	GtkBuilder *builder;
	bool stop_while_accessing_memory;
	PID pid;
	Map *maps;
	Address memory_view_address;
	Address memory_view_entries; // # of entries to show
	unsigned nmaps;
} State;


// get a file descriptor for reading memory from the process
// returns 0 on failure
static int memory_reader_open(State *state) {
	if (state->pid) {
		if (state->stop_while_accessing_memory) {
			if (kill(state->pid, SIGSTOP) == -1) {
				return 0;
			}
		}
		char name[64];
		sprintf(name, "/proc/%lld/mem", (long long)state->pid);
		return open(name, O_RDONLY);
	}
	return 0;
}

static void memory_reader_close(State *state, int fd) {
	if (state->stop_while_accessing_memory) {
		kill(state->pid, SIGCONT);
	}
	if (fd) close(fd);
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
		ssize_t n = read(reader, &memory[idx], nbytes - idx);
		if (n <= 0) break;
		idx += (Address)n;
	}
	return idx;
}

static void display_error_nofmt(State *state, char const *message) {
	GtkWidget *error_box = gtk_message_dialog_new(state->window,
		GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);
	// make sure dialog box is closed when OK is clicked.
	g_signal_connect_swapped(error_box, "response", G_CALLBACK(gtk_widget_destroy), error_box);
	gtk_widget_show_all(error_box);	
}

// this is a macro so we get -Wformat warnings
#define display_error(state, fmt, ...) do { \
	char _buf[1024]; \
	snprintf(_buf, sizeof _buf, fmt, __VA_ARGS__); \
	display_error_nofmt(state, _buf); \
} while (0)

static void update_memory_view(State *state) {
	if (!state->pid || !state->memory_view_address)
		return;
	Address ndisplay = state->memory_view_entries;
	if (ndisplay == 0)
		return;
	GtkBuilder *builder = state->builder;
	uint8_t *mem = calloc(1, ndisplay);
	if (mem) {
		GtkListStore *memory_list = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
		int reader = memory_reader_open(state);
		if (reader) {
			GtkTreeIter iter;
			ndisplay = memory_read_bytes(reader, state->memory_view_address, mem, ndisplay);
			gtk_list_store_clear(memory_list);
			for (Address i = 0; i < ndisplay; ++i) {
				Address addr = state->memory_view_address + i;
				uint8_t value = mem[i];
				char index_str[32], addr_str[32], value_str[32];
				sprintf(index_str, "%" PRIdADDR, i);
				sprintf(addr_str, "%" PRIxADDR, addr);
				sprintf(value_str, "%u", value);
				gtk_list_store_insert_with_values(memory_list, &iter, -1, 0, index_str, 1, addr_str, 2, value_str, -1);
			}
			memory_reader_close(state, reader);
		}
		free(mem);
	} else {
		display_error(state, "Out of memory (trying to display %zu bytes of memory).", ndisplay);
	}
}

G_MODULE_EXPORT void update_configuration(GtkWidget *widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	state->stop_while_accessing_memory = gtk_toggle_button_get_active(
		GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "stop-while-accessing-memory")));
	char const *n_entries_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "memory-display-entries")));
	char *endp;	
	unsigned long n_entries = strtoul(n_entries_text, &endp, 10);
	if (*n_entries_text && !*endp && n_entries != state->memory_view_entries) {
		state->memory_view_entries = n_entries;
		update_memory_view(state);
	}
}

static void update_maps(State *state) {
	free(state->maps); state->maps = NULL;
	
	char maps_name[64];
	sprintf(maps_name, "/proc/%lld/maps", (long long)state->pid);
	FILE *maps_file = fopen(maps_name, "rb");
	if (maps_file) {
		char line[256];
		size_t capacity = 0;
		while (fgets(line, sizeof line, maps_file))
			++capacity;
		rewind(maps_file);
		Map *maps = state->maps = calloc(capacity, sizeof *maps);
		unsigned nmaps = 0;
		if (maps) {
			while (fgets(line, sizeof line, maps_file)) {
				Address addr_lo, addr_hi;
				char protections[8];
				if (sscanf(line, "%" SCNxADDR "-%" SCNxADDR " %8s", &addr_lo, &addr_hi, protections) == 3 && nmaps < capacity) {
					if (strcmp(protections, "rw-p") == 0) { // @TODO(eventually): make this configurable
						Map *map = &maps[nmaps++];
						map->lo = addr_lo;
						map->size = addr_hi - addr_lo;
					}
				}
			}
			state->nmaps = nmaps;
		} else {
			display_error(state, "Not enough memory to hold map metadata (%zu items)", capacity);
		}
	} else {
		display_error(state, "Couldn't open %s: %s", maps_name, strerror(errno));
	}
}

// the user entered a PID.
G_MODULE_EXPORT void select_pid(GtkButton *button, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkEntry *pid = GTK_ENTRY(gtk_builder_get_object(builder, "pid"));
	char const *pid_text = gtk_entry_get_text(pid);
	char *end;
	long long pid_number = strtoll(pid_text, &end, 10);
	if (*pid_text != '\0' && *end == '\0') {
		char dirname[64];
		sprintf(dirname, "/proc/%lld", pid_number);
		int dir = open(dirname, O_DIRECTORY|O_RDONLY);
		if (dir == -1) {
			display_error(state, "Error opening %s: %s", dirname, strerror(errno));
		} else {
			printf("PID: %lld\n", pid_number);
			int cmdline = openat(dir, "cmdline", O_RDONLY);
			char process_name[64] = {0};
			if (cmdline != -1) {
				// the contents of cmdline, up to the first null byte is argv[0],
				// which should be the name of the process
				read(cmdline, process_name, sizeof process_name - 1);	
				close(cmdline);
			}
			if (*process_name == '\0') { // handles unable to open/unable to read/empty cmdline
				strcpy(process_name, "Unknown process.");
			}
			GtkLabel *process_name_label = GTK_LABEL(gtk_builder_get_object(builder, "process-name"));
			gtk_label_set_text(process_name_label, process_name);
			state->pid = (PID)pid_number;
			close(dir);
			update_maps(state);
			if (state->nmaps) {
				// display whatever's in the first mapping
				Map *first_map = &state->maps[0];
				state->memory_view_address = first_map->lo;
				update_memory_view(state);
			}
		}
	}
}

static void on_activate(GtkApplication *app, gpointer user_data) {
	State *state = user_data;
	GError *error = NULL;
	GtkBuilder *builder = gtk_builder_new();
	if (!gtk_builder_add_from_file(builder, "ui.glade", &error)) {
		g_printerr("Error loading UI: %s\n", error->message);
		g_clear_error(&error);
		exit(EXIT_FAILURE);
	}
	state->builder = builder;
	gtk_builder_connect_signals(builder, state);
	
	GtkWindow *window = GTK_WINDOW(gtk_builder_get_object(builder, "window"));
	state->window = window;
	gtk_window_set_application(window, app);
	update_configuration(NULL, state);
	
	gtk_widget_show_all(GTK_WIDGET(window));
}

int main(int argc, char **argv) {
	GtkApplication *app = gtk_application_new("com.pommicket.pokemem", G_APPLICATION_FLAGS_NONE);
	State state = {0};
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
