#include "base.h"
#include "unicode.h"
#include "data.c"
#include "memory.c"

static SearchType search_type_from_str(char const *str) {
	if (strcmp(str, "enter-value") == 0) {
		return SEARCH_ENTER_VALUE;
	} else if (strcmp(str, "same-different") == 0) {
		return SEARCH_SAME_DIFFERENT;
	}
	assert(0);
	return 0xff;
}

// pass config_potentially_changed = false if there definitely hasn't been an update to the target address
// (this is used by auto-refresh so we don't have to clear and re-make the list store each time, which would screw up selection)
static void update_memory_view(State *state, bool config_potentially_changed) {
	if (!state->pid)
		return;
	GtkBuilder *builder = state->builder;
	GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
	uint32_t ndisplay = state->memory_view_entries;
	if (config_potentially_changed)
		gtk_list_store_clear(store);
	if (ndisplay == 0)
		return;
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	void *mem = calloc(item_size, ndisplay);
	if (mem) {
		int reader = memory_reader_open(state);
		if (reader) {
			GtkTreeIter iter;
			ndisplay = (uint32_t)memory_read_bytes(reader, state->memory_view_address, mem, ndisplay * item_size);
			gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, "0");
			char *value = mem;
			Address addr = state->memory_view_address;
			for (Address i = 0; i < ndisplay; i += 1, addr += item_size, value += item_size) {
				char index_str[32], addr_str[32], value_str[32];
				sprintf(index_str, "%" PRIdADDR, i);
				sprintf(addr_str, "%" PRIxADDR, addr);
				data_to_str(value, data_type, value_str, sizeof value_str);
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
		display_error(state, "Out of memory (trying to display %" PRIu32 " bytes of memory).", ndisplay);
	}
}

static void bytes_to_text(uint64_t nbytes, char *out, size_t out_size) {
	if (nbytes == 0)
		snprintf(out, out_size, "0");
	else if (nbytes < (1ul<<10))
		snprintf(out, out_size, "%uB", (unsigned)nbytes);
	else if (nbytes < (1ul<<20))
		snprintf(out, out_size, "%.1fKB", (double)nbytes / (double)(1ul<<10));
	else if (nbytes < (1ul<<30))
		snprintf(out, out_size, "%.1fMB", (double)nbytes / (double)(1ul<<20));
	else
		snprintf(out, out_size, "%.1fGB", (double)nbytes / (double)(1ul<<30));
}

// returns the name of the selected GtkRadioButton in the group whose leader's name is group_name
// (or NULL if none is, which shouldn't usually happen)
static char const *radio_group_get_selected(State *state, char const *group_name) {
	GtkRadioButton *leader = GTK_RADIO_BUTTON(gtk_builder_get_object(state->builder, group_name));
	for (GSList *l = gtk_radio_button_get_group(leader);
		l; l = l->next) {
		GtkToggleButton *button = l->data;
		if (gtk_toggle_button_get_active(button)) {
			return gtk_widget_get_name(GTK_WIDGET(button));
		}
	}
	return NULL;
}

G_MODULE_EXPORT void update_configuration(GtkWidget *_widget, gpointer user_data) {
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
		state->memory_view_entries = (uint32_t)n_entries;
		update_memview = true;
	}
	char const *address_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "address")));
	unsigned long address = strtoul(address_text, &endp, 16);
	if (*address_text && !*endp && address != state->memory_view_address) {
		state->memory_view_address = address;
		update_memview = true;
	}
	char const *data_type_str = radio_group_get_selected(state, "type-u8");
	DataType data_type = data_type_from_name(data_type_str);
	if (state->data_type != data_type) {
		state->data_type = data_type;
		update_memview = true;
	}
	
	// update memory/disk estimates for search
	GtkLabel *memory_label = GTK_LABEL(gtk_builder_get_object(builder, "required-memory"));
	GtkLabel *disk_label   = GTK_LABEL(gtk_builder_get_object(builder, "required-disk"));
	char const *search_type_str = radio_group_get_selected(state, "enter-value");
	SearchType search_type = search_type_from_str(search_type_str);
	state->search_type = search_type;
	if (state->pid) {
		size_t item_size = data_type_size(data_type);
		Address total_memory = state->total_memory;
		Address total_items = total_memory / item_size;
		Address memory_usage = total_items / 8; // 1 bit per item
		{
			char text[32];
			bytes_to_text(memory_usage, text, sizeof text);
			gtk_label_set_text(memory_label, text);
		}
		Address disk_usage = 0;
		switch (search_type) {
		case SEARCH_ENTER_VALUE:
			// no disk space needed
			break;
		case SEARCH_SAME_DIFFERENT:
			// we need to store all of memory on disk for this kind of search
			disk_usage = total_memory;
			break;
		}
		
		{
			char text[32];
			bytes_to_text(disk_usage, text, sizeof text);
			gtk_label_set_text(disk_label, text);
		}
	} else {
		gtk_label_set_text(memory_label, "N/A");
		gtk_label_set_text(disk_label,   "N/A");
	}
	
	if (update_memview) {
		update_memory_view(state, true);
	}
}

// update the memory maps for the current process (state->maps)
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
		state->total_memory = 0;
		if (maps) {
			while (fgets(line, sizeof line, maps_file)) {
				Address addr_lo, addr_hi;
				char protections[8];
				if (sscanf(line, "%" SCNxADDR "-%" SCNxADDR " %8s", &addr_lo, &addr_hi, protections) == 3 && nmaps < capacity) {
					if (strcmp(protections, "rw-p") == 0) { // @TODO(eventually): make this configurable
						Map *map = &maps[nmaps++];
						map->lo = addr_lo;
						map->size = addr_hi - addr_lo;
						state->total_memory += map->size;
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
G_MODULE_EXPORT void select_pid(GtkButton *_button, gpointer user_data) {
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
			update_configuration(NULL, state); // we need to do this to update the search resource usage estimates
			if (state->nmaps) {
				// display whatever's in the first mapping
				Map *first_map = &state->maps[0];
				state->memory_view_address = first_map->lo;
				update_memory_view(state, true);
			}
		}
	}
}

// a value in memory was edited
G_MODULE_EXPORT void memory_edited(GtkCellRendererText *_renderer, char *path, char *new_text, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	Address idx = (Address)atol(path);
	Address addr = state->memory_view_address + idx * item_size;
	state->editing_memory = -1;
	uint64_t value = 0;
	if (data_from_str(new_text, data_type, &value)) {
		int writer = memory_writer_open(state);
		if (writer) {
			// write the value
			bool success = memory_write_bytes(writer, addr, (uint8_t const *)&value, item_size) == item_size;
			memory_writer_close(state, writer);
			if (success) {
				GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
				GtkTreeIter iter;
				char value_str[32];
				// convert back to a string (so new_text = "0.10" becomes value_str = "0.1", etc.)
				data_to_str(&value, data_type, value_str, sizeof value_str);
				gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path);
				gtk_list_store_set(store, &iter, 2, value_str, -1);
			}
			
		}
	}
}

G_MODULE_EXPORT void refresh_memory(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	update_memory_view(state, true);
}

G_MODULE_EXPORT void memory_editing_started(GtkCellRenderer *_renderer, GtkCellEditable *editable, char *path, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = atol(path);
}

G_MODULE_EXPORT void memory_editing_canceled(GtkCellRenderer *_renderer, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = -1;
}

G_MODULE_EXPORT void search_start(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	SearchType search_type = state->search_type;
	gtk_widget_hide(GTK_WIDGET(gtk_builder_get_object(builder, "pre-search")));
	gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-common")));
	switch (search_type) {
	case SEARCH_ENTER_VALUE:
		gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-enter-value")));
		break;
	case SEARCH_SAME_DIFFERENT:
		gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-same-different")));
		break;
	}
}

// this function is run once per frame
static gboolean frame_callback(gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkToggleButton *auto_refresh = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "auto-refresh"));
	
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
