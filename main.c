#include <gtk/gtk.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>

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
	long editing_memory; // index of memory value being edited, or -1 if none is
	PID pid;
	Map *maps;
	Address memory_view_address;
	Address memory_view_entries; // # of entries to show
	unsigned nmaps;
} State;

static void display_dialog_box_nofmt(State *state, GtkMessageType type, char const *message) {
	GtkWidget *box = gtk_message_dialog_new(state->window,
		GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
		type, GTK_BUTTONS_OK, "%s", message);
	// make sure dialog box is closed when OK is clicked.
	g_signal_connect_swapped(box, "response", G_CALLBACK(gtk_widget_destroy), box);
	gtk_widget_show_all(box);	
}

// this is a macro so we get -Wformat warnings
#define display_dialog_box(state, type, fmt, ...) do { \
	char _buf[1024]; \
	snprintf(_buf, sizeof _buf, fmt, __VA_ARGS__); \
	display_dialog_box_nofmt(state, type, _buf); \
} while (0)
#define display_error(state, fmt, ...) display_dialog_box(state, GTK_MESSAGE_ERROR, fmt, __VA_ARGS__)
#define display_info(state, fmt, ...) display_dialog_box(state, GTK_MESSAGE_INFO, fmt, __VA_ARGS__)
#define display_info_nofmt(state, message) display_dialog_box_nofmt(state, GTK_MESSAGE_INFO, message)



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
		ssize_t n = read(reader, &memory[idx], nbytes - idx);
		if (n <= 0) break;
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

// pass config_potentially_changed = false if there definitely hasn't been an update to the target address
// (this is used by auto-refresh so we don't have to clear and re-make the list store each time, which would screw up selection)
static void update_memory_view(State *state, bool config_potentially_changed) {
	if (!state->pid)
		return;
	GtkBuilder *builder = state->builder;
	GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
	Address ndisplay = state->memory_view_entries;
	if (config_potentially_changed)
		gtk_list_store_clear(store);
	if (ndisplay == 0)
		return;
	uint8_t *mem = calloc(1, ndisplay);
	if (mem) {
		int reader = memory_reader_open(state);
		if (reader) {
			GtkTreeIter iter;
			ndisplay = memory_read_bytes(reader, state->memory_view_address, mem, ndisplay);
			gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, "0");
			for (Address i = 0; i < ndisplay; ++i) {
				Address addr = state->memory_view_address + i;
				uint8_t value = mem[i];
				char index_str[32], addr_str[32], value_str[32];
				sprintf(index_str, "%" PRIdADDR, i);
				sprintf(addr_str, "%" PRIxADDR, addr);
				sprintf(value_str, "%u", value);
				if (config_potentially_changed) {
					gtk_list_store_insert_with_values(store, &iter, -1, 0, index_str, 1, addr_str, 2, value_str, -1);
				} else {
					if (i != (Address)state->editing_memory)
						gtk_list_store_set(store, &iter, 0, index_str, 1, addr_str, 2, value_str, -1);
					if (!gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter))
						config_potentially_changed = true; // could happen if a map grows
				}
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
	bool update_memview = false;
	unsigned long n_entries = strtoul(n_entries_text, &endp, 10);
	if (*n_entries_text && !*endp && n_entries != state->memory_view_entries) {
		state->memory_view_entries = n_entries;
		update_memview = true;
	}
	char const *address_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "address")));
	unsigned long address = strtoul(address_text, &endp, 16);
	if (*address_text && !*endp && address != state->memory_view_address) {
		state->memory_view_address = address;
		update_memview = true;
	}
	if (update_memview) {
		update_memory_view(state, true);
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
				update_memory_view(state, true);
			}
		}
	}
}

G_MODULE_EXPORT void memory_edited(GtkCellRendererText *renderer, char *path, char *new_text, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	Address idx = (Address)atol(path);
	Address addr = state->memory_view_address + idx;
	char *endp;
	long value = strtol(new_text, &endp, 10);
	state->editing_memory = -1;
	if (*new_text && *endp == '\0' && value >= 0 && value < 256) {
		uint8_t byte = (uint8_t)value;
		int writer = memory_writer_open(state);
		if (writer) {
			bool success = memory_write_byte(writer, addr, byte) == 1;
			memory_writer_close(state, writer);
			if (success) {
				GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
				GtkTreeIter iter;
				char value_str[16];
				sprintf(value_str, "%u", byte);
				gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path);
				gtk_list_store_set(store, &iter, 2, value_str, -1);
			}
			
		}
	}
}

G_MODULE_EXPORT void memory_row_activated(GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkToggleButton *auto_refresh = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "auto-refresh"));
	gtk_toggle_button_set_active(auto_refresh, 0);
}

G_MODULE_EXPORT void refresh_memory(GtkWidget *widget, gpointer user_data) {
	State *state = user_data;
	update_memory_view(state, true);
}

G_MODULE_EXPORT void memory_editing_started(GtkCellRenderer *renderer, GtkCellEditable *editable, char *path, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = atol(path);
}

G_MODULE_EXPORT void memory_editing_canceled(GtkCellRenderer *renderer, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = -1;
}

// this function is run once per frame
static gboolean frame_callback(gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkToggleButton *auto_refresh = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "auto-refresh"));
#if 0
	GtkWidget *memory_view = GTK_WIDGET(gtk_builder_get_object(builder, "memory-view"));
	for (GtkWidget *focus_widget = gtk_window_get_focus(state->window);
		focus_widget;
		focus_widget = gtk_widget_get_parent(focus_widget)) {
		if (focus_widget == memory_view) {
			// do not allow auto-refresh while potentially editing memory
			gtk_toggle_button_set_active(auto_refresh, 0);
		}
	}
#endif
	
	if (gtk_toggle_button_get_active(auto_refresh)) {
		update_memory_view(state, false);
	}
	return 1;
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
	
	g_timeout_add(16, frame_callback, state);
	
	gtk_widget_show_all(GTK_WIDGET(window));
}

int main(int argc, char **argv) {
	GtkApplication *app = gtk_application_new("com.pommicket.pokemem", G_APPLICATION_FLAGS_NONE);
	State state = {0};
	state.editing_memory = -1;
	g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
	int status = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return status;
}
