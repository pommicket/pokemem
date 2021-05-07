// @TODO:
// - same/different search
// - configure which memory to look at (see "rw-p")
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

static void update_memory_view(State *state, bool addresses_need_updating) {
	GtkBuilder *builder = state->builder;
	GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
	GtkTreeModel *tree_model = GTK_TREE_MODEL(store);
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	
	if (!state->pid) {
		gtk_list_store_clear(store);
		return;
	}
	
	if (addresses_need_updating) {
		gtk_list_store_clear(store);
		if (state->pid) {
			uint64_t *search_candidates = state->search_candidates;
			Address address = state->memory_view_address;
			bool show_candidates = search_candidates && !address;
			unsigned n_items = state->memory_view_n_items;
			if (show_candidates) {
				// show the search candidates
				uint32_t candidate_idx = 0;
				uint64_t *candidates = state->search_candidates;
				Address bitset_index = 0;
				for (unsigned m = 0; m < state->nmaps; ++m) {
					Map *map = &state->maps[m];
					for (Address i = 0; i < map->size / item_size; ) {
						if (i % 64 == 0 && candidates[bitset_index / 64] == 0) {
							// this stretch of 64 has no candidates.
							i += 64;
							bitset_index += 64;
						} else {
							if (candidates[bitset_index / 64] & MASK64(bitset_index % 64)) {
								// a candidate!
								Address addr = map->lo + i * item_size;
								char idx_str[32], addr_str[32];
								sprintf(idx_str, "%u", candidate_idx);
								sprintf(addr_str, "%" PRIxADDR, addr);
								gtk_list_store_insert_with_values(store, NULL, -1, 0, idx_str, 1, addr_str, 2, "", -1);
								++candidate_idx;
								if (candidate_idx >= n_items) goto done;
							}
							++i;
							++bitset_index;
						}
					}
				}
				done:;
			} else {
				if (address) {
					// show `n_items` items starting from `address`
					for (unsigned i = 0; i < n_items; ++i) {
						char idx_str[32], addr_str[32];
						Address addr = address + i * item_size;
						sprintf(idx_str, "%u", i);
						sprintf(addr_str, "%" PRIxADDR, addr);
						gtk_list_store_insert_with_values(store, NULL, -1, 0, idx_str, 1, addr_str, 2, "", -1);
					}
				}
			}
		}
	}
	
	GtkTreeIter iter;
	if (gtk_tree_model_get_iter_first(tree_model, &iter)) {
		int reader = memory_reader_open(state);
		if (reader) {
			int i = 0;
			do {
				if (i != state->editing_memory) {
					gchararray addr_str = NULL;
					gtk_tree_model_get(tree_model, &iter, 1, &addr_str, -1);
					Address addr = 0;
					sscanf(addr_str, "%" SCNxADDR, &addr);
					g_free(addr_str);
					uint64_t value = 0;
					size_t nread = memory_read_bytes(reader, addr, (uint8_t *)&value, item_size);
					char value_str[32];
					if (nread == item_size)
						data_to_str(&value, data_type, value_str, sizeof value_str);
					else
						strcpy(value_str, "N/A");
					gtk_list_store_set(store, &iter, 2, value_str, -1);
				}
				++i;
			} while (gtk_tree_model_iter_next(tree_model, &iter));
		}
		memory_reader_close(state, reader);
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
	char const *n_items_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "memory-n-items")));
	char *endp;
	unsigned n_items = (unsigned)strtoul(n_items_text, &endp, 10);
	if (*endp || !*n_items_text) n_items = 0;
	if (n_items >= 1000) n_items = 999;
	char const *address_text = gtk_entry_get_text(
		GTK_ENTRY(gtk_builder_get_object(builder, "address")));
	unsigned long address = strtoul(address_text, &endp, 16);
	if (*endp || !*address_text) address = 0;
	
	char const *data_type_str = radio_group_get_selected(state, "type-u8");
	DataType data_type = data_type_from_name(data_type_str);
	
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
	
	static bool prev_candidates;
	static PID prev_pid;
	
	uint64_t *search_candidates = state->search_candidates;
	if (n_items != state->memory_view_n_items || (!!search_candidates) != prev_candidates || address != state->memory_view_address || data_type != state->data_type || state->pid != prev_pid) {
		// we need to update the addresses in the memory view.
		prev_candidates = !!search_candidates;
		state->memory_view_n_items = n_items;
		state->memory_view_address = address;
		state->data_type = data_type;
		prev_pid = state->pid;
		
		update_memory_view(state, true);
	}
}

G_MODULE_EXPORT void set_all(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkEntry *value_entry = GTK_ENTRY(gtk_builder_get_object(builder, "set-all-value"));
	uint64_t value = 0;
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	// parse the value
	if (data_from_str(gtk_entry_get_text(value_entry), data_type, &value)) {
		GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
		GtkTreeModel *tree_model = GTK_TREE_MODEL(store);
		GtkTreeIter iter;
		if (gtk_tree_model_get_iter_first(tree_model, &iter)) {
			int writer = memory_writer_open(state);
			if (writer) {
				do { // for each row in the memory view,
					// extract address
					gchararray addr_str;
					gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 1, &addr_str, -1);
					Address addr = (Address)strtoull(addr_str, NULL, 16);
					g_free(addr_str);
					// set memory to value
					memory_write_bytes(writer, addr, (uint8_t const *)&value, item_size);
				} while (gtk_tree_model_iter_next(tree_model, &iter));
				memory_writer_close(state, writer);
			}
		}
	}
}

// update the memory maps for the current process (state->maps)
// returns true on success
static bool update_maps(State *state) {
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
			return true;
		} else {
			display_error(state, "Not enough memory to hold map metadata (%zu items)", capacity);
		}
	} else {
		display_error(state, "Couldn't open %s: %s", maps_name, strerror(errno));
	}
	return false;
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
			if (update_maps(state)) {
				if (state->nmaps) {
					GtkEntry *address_entry = GTK_ENTRY(gtk_builder_get_object(builder, "address"));
					Address addr = state->maps[0].lo;
					char addr_text[32];
					sprintf(addr_text, "%" PRIxADDR, addr);
					gtk_entry_set_text(address_entry, addr_text);
				}
				update_configuration(NULL, state); // we need to do this to update the search resource usage estimates, etc.
				update_memory_view(state, true);
				// only allow searching once a process has been selected
				gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-box")));
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
	GtkListStore *store = GTK_LIST_STORE(gtk_builder_get_object(builder, "memory"));
	GtkTreeIter iter;
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(store), &iter, path);
	gchararray addr_str;
	// get address from store
	gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 1, &addr_str, -1);
	Address addr = strtoull(addr_str, NULL, 16);
	g_free(addr_str);
	state->editing_memory = -1;
	uint64_t value = 0;
	if (data_from_str(new_text, data_type, &value)) {
		int writer = memory_writer_open(state);
		if (writer) {
			// write the value
			bool success = memory_write_bytes(writer, addr, (uint8_t const *)&value, item_size) == item_size;
			memory_writer_close(state, writer);
			if (success) {
				char value_str[32];
				// convert back to a string (so new_text = "0.10" becomes value_str = "0.1", etc.)
				data_to_str(&value, data_type, value_str, sizeof value_str);
				gtk_list_store_set(store, &iter, 2, value_str, -1);
				
			}
			
		}
	}
}

G_MODULE_EXPORT void refresh_memory(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	update_configuration(NULL, state); // just in case they changed the number of items or something
	update_memory_view(state, false);
}

G_MODULE_EXPORT void memory_editing_started(GtkCellRenderer *_renderer, GtkCellEditable *editable, char *path, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = atol(path);
}

G_MODULE_EXPORT void memory_editing_canceled(GtkCellRenderer *_renderer, gpointer user_data) {
	State *state = user_data;
	state->editing_memory = -1;
}

G_MODULE_EXPORT void memory_view_key_press(GtkWidget *widget, GdkEvent *event, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GdkEventKey *key_event = (GdkEventKey *)event;
	if (key_event->keyval == GDK_KEY_Delete) {
		uint64_t *search_candidates = state->search_candidates;
		if (search_candidates && !state->memory_view_address) {
			// allow deleting candidates with the delete key
			GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
			GtkTreeModel *tree_model = GTK_TREE_MODEL(gtk_builder_get_object(builder, "memory"));
			GtkListStore *list_store = GTK_LIST_STORE(tree_model);
			GtkTreeSelection *selection = gtk_tree_view_get_selection(tree_view);
			GList *selected_rows = gtk_tree_selection_get_selected_rows(selection, NULL);
			for (GList *list = selected_rows; list; list = list->next) {
				GtkTreePath *path = list->data;
				GtkTreeIter iter;
				if (gtk_tree_model_get_iter(tree_model, &iter, path)) {
					gchararray addr_str;
					gtk_tree_model_get(tree_model, &iter, 1, &addr_str, -1);
					Address addr = (Address)strtoull(addr_str, NULL, 16);
					g_free(addr_str);
					gtk_list_store_remove(list_store, &iter);
					Address bitset_idx = 0;
					for (unsigned m = 0; m < state->nmaps; ++m) {
						Map *map = &state->maps[m];
						if (addr >= map->lo && addr < map->lo + map->size) {
							bitset_idx += addr - map->lo;
							// remove this candidate
							search_candidates[bitset_idx / 64] &= ~MASK64(bitset_idx % 64);
							break;
						} else {
							bitset_idx += map->size;
						}
					}
				}
			}
			g_list_free_full(selected_rows, (GDestroyNotify)gtk_tree_path_free);
		}
	}
}


static void update_candidates(State *state) {
	GtkBuilder *builder = state->builder;
	uint64_t *candidates = state->search_candidates;
	size_t item_size = data_type_size(state->data_type);
	Address entries = state->total_memory / (64 * item_size);
	Address ncandidates = 0;
	for (Address i = 0; i < entries; ++i) {
		ncandidates += (unsigned)__builtin_popcountll(candidates[i]);
	}
	{
		GtkLabel *ncandidates_label = GTK_LABEL(gtk_builder_get_object(builder, "candidates-left"));
		char text[32];
		sprintf(text, "%llu", (unsigned long long)ncandidates);
		gtk_label_set_text(ncandidates_label, text);
	}
}

G_MODULE_EXPORT void search_start(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	if (update_maps(state)) {
		GtkBuilder *builder = state->builder;
		SearchType search_type = state->search_type;
		DataType data_type = state->data_type;
		size_t item_size = data_type_size(data_type);
		// state->total_memory should always be a multiple of the page size, which is definitely a multiple of 64 * 8 = 512.
		assert(state->total_memory % 512 == 0);
		uint64_t *candidates = state->search_candidates = malloc(state->total_memory / (8 * item_size));
		if (candidates) {
			gtk_widget_hide(GTK_WIDGET(gtk_builder_get_object(builder, "pre-search")));
			gtk_widget_set_sensitive(GTK_WIDGET(gtk_builder_get_object(builder, "data-type-box")), 0);
			gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-common")));
			gtk_label_set_text(GTK_LABEL(gtk_builder_get_object(builder, "steps-completed")), "0");
			gtk_entry_set_text(GTK_ENTRY(gtk_builder_get_object(builder, "address")), "");
			memset(candidates, 0xff, state->total_memory / (8 * item_size));
			switch (search_type) {
			case SEARCH_ENTER_VALUE:
				gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-enter-value")));
				break;
			case SEARCH_SAME_DIFFERENT:
				gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "search-same-different")));
				break;
			}
			update_configuration(NULL, state);
			update_candidates(state);
		} else {
			display_error_nofmt(state, "Not enough memory available for search.");
		}
	}
	
}

G_MODULE_EXPORT void search_update(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	GtkWindow *window = state->window;
	GtkWidget *search_box = GTK_WIDGET(gtk_builder_get_object(builder, "search-box"));
	
	// disabling search-box can mess up the focus, it turns out
	state->prev_focus = gtk_window_get_focus(window);
	gtk_widget_set_sensitive(search_box, 0); // temporarily disable everything search-related so that you don't accidentally queue up a bunch of updates while it's loading. it will be reset on the next frame_callback.
	
	DataType data_type = state->data_type;
	size_t item_size = data_type_size(data_type);
	SearchType search_type = state->search_type;
	uint64_t *candidates = state->search_candidates;
	int memory_reader = memory_reader_open(state);
	bool success = true;
	
	if (memory_reader) {
		switch (search_type) {
		case SEARCH_ENTER_VALUE: {
			uint64_t value;
			GtkEntry *value_entry = GTK_ENTRY(gtk_builder_get_object(builder, "current-value"));
			if (data_from_str(gtk_entry_get_text(value_entry), data_type, &value)) {
				Address bitset_index = 0;
				for (unsigned m = 0; m < state->nmaps; ++m) {
					// there is some kinda complicated code here to skip over large regions of
					// eliminated addresses. specifically, we can skip 64 bytes at a time, because
					// of our bitset.
					Map const *map = &state->maps[m];
					Address addr_lo = map->lo;
					Address n_items = map->size / item_size;
					Address start = 0;
					while (start < n_items) {
						while (start < n_items) {
							if (candidates[bitset_index/64])
								break;
							start += 64;
							bitset_index += 64;
						}
						Address end = start, bitset_index_end = bitset_index;
						
						while (end < n_items) {
							if (candidates[bitset_index_end/64] == 0)
								break;
							end += 64;
							bitset_index_end += 64;
						}
						
						Address run_addr = addr_lo + start * item_size;
						// we have a "run" of possible candidates from `start` to `end`.
						Address run_size = end - start;
						Address bytes_left = run_size * item_size;
						// this "run" could be pretty long, so let's do it in chunks.
						uint64_t chunk[512]; // (make sure this is as aligned as possible)
						while (bytes_left > 0) {
							size_t this_chunk_bytes = sizeof chunk;
							if (this_chunk_bytes > bytes_left)
								this_chunk_bytes = bytes_left;
							
							Address chunk_addr = run_addr + run_size * item_size - bytes_left;
							memset(chunk, 0, sizeof chunk); // if we can't read the memory, treat it as 0
							memory_read_bytes(memory_reader, chunk_addr, (uint8_t *)chunk, this_chunk_bytes);
							size_t this_chunk_items = this_chunk_bytes / item_size;
							for (size_t i = 0; i < this_chunk_items; ++i) {
								void const *value_here = &((uint8_t const *)chunk)[i * item_size];
								if (!data_equal(data_type, &value, value_here)) {
									// eliminate this candidate
									candidates[bitset_index/64] &= ~MASK64(bitset_index % 64);
								}
								++bitset_index;
							}
							
							bytes_left -= this_chunk_bytes;
						}
						assert(bitset_index == bitset_index_end);
						start = end;
					}
				}
			} else success = false;
		} break;
		case SEARCH_SAME_DIFFERENT:
			// @TODO
			display_error_nofmt(state, "not implemented");
			break;
		}
		memory_reader_close(state, memory_reader);
	} else success = false;
	
	if (success) {
		GtkLabel *steps_completed_label = GTK_LABEL(gtk_builder_get_object(builder, "steps-completed"));
		long steps_completed = 1 + atol(gtk_label_get_text(steps_completed_label));
		{
			char text[32];
			sprintf(text, "%ld", steps_completed);
			gtk_label_set_text(steps_completed_label, text);
		}
		update_candidates(state);
		update_memory_view(state, true);
	}
	
}

G_MODULE_EXPORT void search_stop(GtkWidget *_widget, gpointer user_data) {
	State *state = user_data;
	GtkBuilder *builder = state->builder;
	free(state->search_candidates);
	state->search_candidates = NULL;
	gtk_widget_hide(GTK_WIDGET(gtk_builder_get_object(builder, "search-common")));
	gtk_widget_hide(GTK_WIDGET(gtk_builder_get_object(builder, "search-enter-value")));
	gtk_widget_hide(GTK_WIDGET(gtk_builder_get_object(builder, "search-same-different")));
	gtk_widget_show(GTK_WIDGET(gtk_builder_get_object(builder, "pre-search")));
	gtk_widget_set_sensitive(GTK_WIDGET(gtk_builder_get_object(builder, "data-type-box")), 1);
}

// this function is run once per frame
static gboolean frame_callback(gpointer user_data) {
	State *state = user_data;
	GtkWindow *window = state->window;
	GtkBuilder *builder = state->builder;
	
	GtkWidget *search_box = GTK_WIDGET(gtk_builder_get_object(builder, "search-box"));
	
	// sometimes we disable search-box. see search_update.
	if (!gtk_widget_get_sensitive(search_box)) {
		static int frame_counter;
		if (frame_counter) {
			gtk_widget_set_sensitive(search_box, 1);
			gtk_window_set_focus(window, state->prev_focus);
			frame_counter = 0;
		} else {
			++frame_counter;
		}
	}
	
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
	
	g_timeout_add(200, frame_callback, state);
	
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
