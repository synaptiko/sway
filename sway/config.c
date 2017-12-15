#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <wordexp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <libinput.h>
#include <limits.h>
#include <float.h>
#include <dirent.h>
#include <strings.h>
#ifdef __linux__
#include <linux/input-event-codes.h>
#elif __FreeBSD__
#include <dev/evdev/input-event-codes.h>
#endif
#include <wlr/types/wlr_output.h>
#include "sway/input/input-manager.h"
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/layout.h"
#include "readline.h"
#include "stringop.h"
#include "list.h"
#include "log.h"

struct sway_config *config = NULL;

void free_config(struct sway_config *config) {
	// TODO
}

static void config_defaults(struct sway_config *config) {
	if (!(config->symbols = create_list())) goto cleanup;
	if (!(config->modes = create_list())) goto cleanup;
	if (!(config->bars = create_list())) goto cleanup;
	if (!(config->workspace_outputs = create_list())) goto cleanup;
	if (!(config->pid_workspaces = create_list())) goto cleanup;
	if (!(config->criteria = create_list())) goto cleanup;
	if (!(config->no_focus = create_list())) goto cleanup;
	if (!(config->input_configs = create_list())) goto cleanup;
	if (!(config->seat_configs = create_list())) goto cleanup;
	if (!(config->output_configs = create_list())) goto cleanup;

	if (!(config->cmd_queue = create_list())) goto cleanup;

	if (!(config->current_mode = malloc(sizeof(struct sway_mode))))
		goto cleanup;
	if (!(config->current_mode->name = malloc(sizeof("default")))) goto cleanup;
	strcpy(config->current_mode->name, "default");
	if (!(config->current_mode->bindings = create_list())) goto cleanup;
	list_add(config->modes, config->current_mode);

	config->floating_mod = 0;
	config->dragging_key = BTN_LEFT;
	config->resizing_key = BTN_RIGHT;
	if (!(config->floating_scroll_up_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_down_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_left_cmd = strdup(""))) goto cleanup;
	if (!(config->floating_scroll_right_cmd = strdup(""))) goto cleanup;
	config->default_layout = L_NONE;
	config->default_orientation = L_NONE;
	if (!(config->font = strdup("monospace 10"))) goto cleanup;
	// TODO: border
	//config->font_height = get_font_text_height(config->font);

	// floating view
	config->floating_maximum_width = 0;
	config->floating_maximum_height = 0;
	config->floating_minimum_width = 75;
	config->floating_minimum_height = 50;

	// Flags
	config->focus_follows_mouse = true;
	config->mouse_warping = true;
	config->reloading = false;
	config->active = false;
	config->failed = false;
	config->auto_back_and_forth = false;
	config->seamless_mouse = true;
	config->reading = false;
	config->show_marks = true;

	config->edge_gaps = true;
	config->smart_gaps = false;
	config->gaps_inner = 0;
	config->gaps_outer = 0;

	if (!(config->active_bar_modifiers = create_list())) goto cleanup;

	if (!(config->config_chain = create_list())) goto cleanup;
	config->current_config = NULL;

	// borders
	config->border = B_NORMAL;
	config->floating_border = B_NORMAL;
	config->border_thickness = 2;
	config->floating_border_thickness = 2;
	config->hide_edge_borders = E_NONE;

	// border colors
	config->border_colors.focused.border = 0x4C7899FF;
	config->border_colors.focused.background = 0x285577FF;
	config->border_colors.focused.text = 0xFFFFFFFF;
	config->border_colors.focused.indicator = 0x2E9EF4FF;
	config->border_colors.focused.child_border = 0x285577FF;

	config->border_colors.focused_inactive.border = 0x333333FF;
	config->border_colors.focused_inactive.background = 0x5F676AFF;
	config->border_colors.focused_inactive.text = 0xFFFFFFFF;
	config->border_colors.focused_inactive.indicator = 0x484E50FF;
	config->border_colors.focused_inactive.child_border = 0x5F676AFF;

	config->border_colors.unfocused.border = 0x333333FF;
	config->border_colors.unfocused.background = 0x222222FF;
	config->border_colors.unfocused.text = 0x888888FF;
	config->border_colors.unfocused.indicator = 0x292D2EFF;
	config->border_colors.unfocused.child_border = 0x222222FF;

	config->border_colors.urgent.border = 0x2F343AFF;
	config->border_colors.urgent.background = 0x900000FF;
	config->border_colors.urgent.text = 0xFFFFFFFF;
	config->border_colors.urgent.indicator = 0x900000FF;
	config->border_colors.urgent.child_border = 0x900000FF;

	config->border_colors.placeholder.border = 0x000000FF;
	config->border_colors.placeholder.background = 0x0C0C0CFF;
	config->border_colors.placeholder.text = 0xFFFFFFFF;
	config->border_colors.placeholder.indicator = 0x000000FF;
	config->border_colors.placeholder.child_border = 0x0C0C0CFF;

	config->border_colors.background = 0xFFFFFFFF;

	// Security
	if (!(config->command_policies = create_list())) goto cleanup;
	if (!(config->feature_policies = create_list())) goto cleanup;
	if (!(config->ipc_policies = create_list())) goto cleanup;

	return;
cleanup:
	sway_abort("Unable to allocate config structures");
}

static bool file_exists(const char *path) {
	return path && access(path, R_OK) != -1;
}

static char *get_config_path(void) {
	static const char *config_paths[] = {
		"$HOME/.sway/config",
		"$XDG_CONFIG_HOME/sway/config",
		"$HOME/.i3/config",
		"$XDG_CONFIG_HOME/i3/config",
		SYSCONFDIR "/sway/config",
		SYSCONFDIR "/i3/config",
	};

	if (!getenv("XDG_CONFIG_HOME")) {
		char *home = getenv("HOME");
		char *config_home = malloc(strlen(home) + strlen("/.config") + 1);
		if (!config_home) {
			sway_log(L_ERROR, "Unable to allocate $HOME/.config");
		} else {
			strcpy(config_home, home);
			strcat(config_home, "/.config");
			setenv("XDG_CONFIG_HOME", config_home, 1);
			sway_log(L_DEBUG, "Set XDG_CONFIG_HOME to %s", config_home);
			free(config_home);
		}
	}

	wordexp_t p;
	char *path;

	int i;
	for (i = 0; i < (int)(sizeof(config_paths) / sizeof(char *)); ++i) {
		if (wordexp(config_paths[i], &p, 0) == 0) {
			path = strdup(p.we_wordv[0]);
			wordfree(&p);
			if (file_exists(path)) {
				return path;
			}
		}
	}

	return NULL; // Not reached
}

const char *current_config_path;

static bool load_config(const char *path, struct sway_config *config) {
	sway_log(L_INFO, "Loading config from %s", path);
	current_config_path = path;

	struct stat sb;
	if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
		return false;
	}

	if (path == NULL) {
		sway_log(L_ERROR, "Unable to find a config file!");
		return false;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		sway_log(L_ERROR, "Unable to open %s for reading", path);
		return false;
	}

	bool config_load_success = read_config(f, config);
	fclose(f);

	if (!config_load_success) {
		sway_log(L_ERROR, "Error(s) loading config!");
	}

	current_config_path = NULL;
	return true;
}

static int qstrcmp(const void* a, const void* b) {
	return strcmp(*((char**) a), *((char**) b));
}

struct input_config *new_input_config(const char* identifier) {
	struct input_config *input = calloc(1, sizeof(struct input_config));
	if (!input) {
		sway_log(L_DEBUG, "Unable to allocate input config");
		return NULL;
	}
	sway_log(L_DEBUG, "new_input_config(%s)", identifier);
	if (!(input->identifier = strdup(identifier))) {
		free(input);
		sway_log(L_DEBUG, "Unable to allocate input config");
		return NULL;
	}

	input->tap = INT_MIN;
	input->drag_lock = INT_MIN;
	input->dwt = INT_MIN;
	input->send_events = INT_MIN;
	input->click_method = INT_MIN;
	input->middle_emulation = INT_MIN;
	input->natural_scroll = INT_MIN;
	input->accel_profile = INT_MIN;
	input->pointer_accel = FLT_MIN;
	input->scroll_method = INT_MIN;
	input->left_handed = INT_MIN;

	return input;
}

void merge_input_config(struct input_config *dst, struct input_config *src) {
	if (src->identifier) {
		free(dst->identifier);
		dst->identifier = strdup(src->identifier);
	}
	if (src->accel_profile != INT_MIN) {
		dst->accel_profile = src->accel_profile;
	}
	if (src->click_method != INT_MIN) {
		dst->click_method = src->click_method;
	}
	if (src->drag_lock != INT_MIN) {
		dst->drag_lock = src->drag_lock;
	}
	if (src->dwt != INT_MIN) {
		dst->dwt = src->dwt;
	}
	if (src->middle_emulation != INT_MIN) {
		dst->middle_emulation = src->middle_emulation;
	}
	if (src->natural_scroll != INT_MIN) {
		dst->natural_scroll = src->natural_scroll;
	}
	if (src->pointer_accel != FLT_MIN) {
		dst->pointer_accel = src->pointer_accel;
	}
	if (src->scroll_method != INT_MIN) {
		dst->scroll_method = src->scroll_method;
	}
	if (src->send_events != INT_MIN) {
		dst->send_events = src->send_events;
	}
	if (src->tap != INT_MIN) {
		dst->tap = src->tap;
	}
	if (src->xkb_layout) {
		free(dst->xkb_layout);
		dst->xkb_layout = strdup(src->xkb_layout);
	}
	if (src->xkb_model) {
		free(dst->xkb_model);
		dst->xkb_model = strdup(src->xkb_model);
	}
	if (src->xkb_options) {
		free(dst->xkb_options);
		dst->xkb_options = strdup(src->xkb_options);
	}
	if (src->xkb_rules) {
		free(dst->xkb_rules);
		dst->xkb_rules = strdup(src->xkb_rules);
	}
	if (src->xkb_variant) {
		free(dst->xkb_variant);
		dst->xkb_variant = strdup(src->xkb_variant);
	}
}

void free_input_config(struct input_config *ic) {
	if (!ic) {
		return;
	}
	free(ic->identifier);
	free(ic);
}

int input_identifier_cmp(const void *item, const void *data) {
	const struct input_config *ic = item;
	const char *identifier = data;
	return strcmp(ic->identifier, identifier);
}

struct seat_config *new_seat_config(const char* name) {
	struct seat_config *seat = calloc(1, sizeof(struct seat_config));
	if (!seat) {
		sway_log(L_DEBUG, "Unable to allocate seat config");
		return NULL;
	}

	sway_log(L_DEBUG, "new_seat_config(%s)", name);
	seat->name = strdup(name);
	if (!sway_assert(seat->name, "could not allocate name for seat")) {
		return NULL;
	}

	seat->attachments = create_list();
	if (!sway_assert(seat->attachments,
				"could not allocate seat attachments list")) {
		return NULL;
	}

	return seat;
}

struct seat_attachment_config *seat_attachment_config_new() {
	struct seat_attachment_config *attachment =
		calloc(1, sizeof(struct seat_attachment_config));
	if (!attachment) {
		sway_log(L_DEBUG, "cannot allocate attachment config");
		return NULL;
	}
	return attachment;
}

static void seat_attachment_config_free(
		struct seat_attachment_config *attachment) {
	free(attachment->identifier);
	free(attachment);
	return;
}

static struct seat_attachment_config *seat_attachment_config_copy(
		struct seat_attachment_config *attachment) {
	struct seat_attachment_config *copy = seat_attachment_config_new();
	if (!copy) {
		return NULL;
	}

	copy->identifier = strdup(attachment->identifier);

	return copy;
}

static void merge_seat_attachment_config(struct seat_attachment_config *dest,
		struct seat_attachment_config *source) {
	// nothing to merge yet, but there will be some day
}

void merge_seat_config(struct seat_config *dest, struct seat_config *source) {
	if (source->name) {
		free(dest->name);
		dest->name = strdup(source->name);
	}

	for (int i = 0; i < source->attachments->length; ++i) {
		struct seat_attachment_config *source_attachment =
			source->attachments->items[i];
		bool found = false;
		for (int j = 0; j < dest->attachments->length; ++j) {
			struct seat_attachment_config *dest_attachment =
				dest->attachments->items[j];
			if (strcmp(source_attachment->identifier,
						dest_attachment->identifier) == 0) {
				merge_seat_attachment_config(dest_attachment,
					source_attachment);
				found = true;
			}
		}

		if (!found) {
			struct seat_attachment_config *copy =
				seat_attachment_config_copy(source_attachment);
			if (copy) {
				list_add(dest->attachments, copy);
			}
		}
	}
}

void free_seat_config(struct seat_config *seat) {
	if (!seat) {
		return;
	}

	free(seat->name);
	for (int i = 0; i < seat->attachments->length; ++i) {
		struct seat_attachment_config *attachment =
			seat->attachments->items[i];
		seat_attachment_config_free(attachment);
	}

	list_free(seat->attachments);
	free(seat);
}

int seat_name_cmp(const void *item, const void *data) {
	const struct seat_config *sc = item;
	const char *name = data;
	return strcmp(sc->name, name);
}

struct seat_attachment_config *seat_config_get_attachment(
		struct seat_config *seat_config, char *identifier) {
	for (int i = 0; i < seat_config->attachments->length; ++i) {
		struct seat_attachment_config *attachment =
			seat_config->attachments->items[i];
		if (strcmp(attachment->identifier, identifier) == 0) {
			return attachment;
		}
	}

	return NULL;
}

bool load_main_config(const char *file, bool is_active) {
	char *path;
	if (file != NULL) {
		path = strdup(file);
	} else {
		path = get_config_path();
	}

	struct sway_config *old_config = config;
	config = calloc(1, sizeof(struct sway_config));
	if (!config) {
		sway_abort("Unable to allocate config");
	}

	config_defaults(config);
	if (is_active) {
		sway_log(L_DEBUG, "Performing configuration file reload");
		config->reloading = true;
		config->active = true;
	}

	config->current_config = path;
	list_add(config->config_chain, path);

	config->reading = true;

	// Read security configs
	bool success = true;
	DIR *dir = opendir(SYSCONFDIR "/sway/security.d");
	if (!dir) {
		sway_log(L_ERROR,
			"%s does not exist, sway will have no security configuration"
			" and will probably be broken", SYSCONFDIR "/sway/security.d");
	} else {
		list_t *secconfigs = create_list();
		char *base = SYSCONFDIR "/sway/security.d/";
		struct dirent *ent = readdir(dir);
		struct stat s;
		while (ent != NULL) {
			char *_path = malloc(strlen(ent->d_name) + strlen(base) + 1);
			strcpy(_path, base);
			strcat(_path, ent->d_name);
			lstat(_path, &s);
			if (S_ISREG(s.st_mode) && ent->d_name[0] != '.') {
				list_add(secconfigs, _path);
			}
			else {
				free(_path);
			}
			ent = readdir(dir);
		}
		closedir(dir);

		list_qsort(secconfigs, qstrcmp);
		for (int i = 0; i < secconfigs->length; ++i) {
			char *_path = secconfigs->items[i];
			if (stat(_path, &s) || s.st_uid != 0 || s.st_gid != 0 ||
					(((s.st_mode & 0777) != 0644) &&
					(s.st_mode & 0777) != 0444)) {
				sway_log(L_ERROR,
					"Refusing to load %s - it must be owned by root "
					"and mode 644 or 444", _path);
				success = false;
			} else {
				success = success && load_config(_path, config);
			}
		}

		free_flat_list(secconfigs);
	}

	success = success && load_config(path, config);

	if (is_active) {
		config->reloading = false;
	}

	if (old_config) {
		free_config(old_config);
	}
	config->reading = false;

	if (success) {
		// TODO: bar
		//update_active_bar_modifiers();
	}

	return success;
}

static bool load_include_config(const char *path, const char *parent_dir,
		struct sway_config *config) {
	// save parent config
	const char *parent_config = config->current_config;

	char *full_path = strdup(path);
	int len = strlen(path);
	if (len >= 1 && path[0] != '/') {
		len = len + strlen(parent_dir) + 2;
		full_path = malloc(len * sizeof(char));
		if (!full_path) {
			sway_log(L_ERROR,
				"Unable to allocate full path to included config");
			return false;
		}
		snprintf(full_path, len, "%s/%s", parent_dir, path);
	}

	char *real_path = realpath(full_path, NULL);
	free(full_path);

	if (real_path == NULL) {
		sway_log(L_DEBUG, "%s not found.", path);
		return false;
	}

	// check if config has already been included
	int j;
	for (j = 0; j < config->config_chain->length; ++j) {
		char *old_path = config->config_chain->items[j];
		if (strcmp(real_path, old_path) == 0) {
			sway_log(L_DEBUG,
				"%s already included once, won't be included again.",
				real_path);
			free(real_path);
			return false;
		}
	}

	config->current_config = real_path;
	list_add(config->config_chain, real_path);
	int index = config->config_chain->length - 1;

	if (!load_config(real_path, config)) {
		free(real_path);
		config->current_config = parent_config;
		list_del(config->config_chain, index);
		return false;
	}

	// restore current_config
	config->current_config = parent_config;
	return true;
}

bool load_include_configs(const char *path, struct sway_config *config) {
	char *wd = getcwd(NULL, 0);
	char *parent_path = strdup(config->current_config);
	const char *parent_dir = dirname(parent_path);

	if (chdir(parent_dir) < 0) {
		free(parent_path);
		free(wd);
		return false;
	}

	wordexp_t p;

	if (wordexp(path, &p, 0) < 0) {
		free(parent_path);
		free(wd);
		return false;
	}

	char **w = p.we_wordv;
	size_t i;
	for (i = 0; i < p.we_wordc; ++i) {
		load_include_config(w[i], parent_dir, config);
	}
	free(parent_path);
	wordfree(&p);

	// restore wd
	if (chdir(wd) < 0) {
		free(wd);
		sway_log(L_ERROR, "failed to restore working directory");
		return false;
	}

	free(wd);
	return true;
}


bool read_config(FILE *file, struct sway_config *config) {
	bool success = true;
	enum cmd_status block = CMD_BLOCK_END;

	int line_number = 0;
	char *line;
	while (!feof(file)) {
		line = read_line(file);
		if (!line) {
			continue;
		}
		line_number++;
		line = strip_whitespace(line);
		if (line[0] == '#') {
			free(line);
			continue;
		}
		struct cmd_results *res;
		if (block == CMD_BLOCK_COMMANDS) {
			// Special case
			res = config_commands_command(line);
		} else {
			res = config_command(line, block);
		}
		switch(res->status) {
		case CMD_FAILURE:
		case CMD_INVALID:
			sway_log(L_ERROR, "Error on line %i '%s': %s (%s)", line_number,
				line, res->error, config->current_config);
			success = false;
			break;

		case CMD_DEFER:
			sway_log(L_DEBUG, "Defferring command `%s'", line);
			list_add(config->cmd_queue, strdup(line));
			break;

		case CMD_BLOCK_MODE:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_MODE;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_INPUT:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_INPUT;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_SEAT:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_SEAT;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_BAR:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_BAR;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_BAR_COLORS:
			if (block == CMD_BLOCK_BAR) {
				block = CMD_BLOCK_BAR_COLORS;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_COMMANDS:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_COMMANDS;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_IPC:
			if (block == CMD_BLOCK_END) {
				block = CMD_BLOCK_IPC;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_IPC_EVENTS:
			if (block == CMD_BLOCK_IPC) {
				block = CMD_BLOCK_IPC_EVENTS;
			} else {
				sway_log(L_ERROR, "Invalid block '%s'", line);
			}
			break;

		case CMD_BLOCK_END:
			switch(block) {
			case CMD_BLOCK_MODE:
				sway_log(L_DEBUG, "End of mode block");
				config->current_mode = config->modes->items[0];
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_INPUT:
				sway_log(L_DEBUG, "End of input block");
				current_input_config = NULL;
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_SEAT:
				sway_log(L_DEBUG, "End of seat block");
				current_seat_config = NULL;
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_BAR:
				sway_log(L_DEBUG, "End of bar block");
				config->current_bar = NULL;
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_BAR_COLORS:
				sway_log(L_DEBUG, "End of bar colors block");
				block = CMD_BLOCK_BAR;
				break;

			case CMD_BLOCK_COMMANDS:
				sway_log(L_DEBUG, "End of commands block");
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_IPC:
				sway_log(L_DEBUG, "End of IPC block");
				block = CMD_BLOCK_END;
				break;

			case CMD_BLOCK_IPC_EVENTS:
				sway_log(L_DEBUG, "End of IPC events block");
				block = CMD_BLOCK_IPC;
				break;

			case CMD_BLOCK_END:
				sway_log(L_ERROR, "Unmatched }");
				break;

			default:;
			}
		default:;
		}
		free(line);
		free_cmd_results(res);
	}

	return success;
}

char *do_var_replacement(char *str) {
	int i;
	char *find = str;
	while ((find = strchr(find, '$'))) {
		// Skip if escaped.
		if (find > str && find[-1] == '\\') {
			if (find == str + 1 || !(find > str + 1 && find[-2] == '\\')) {
				++find;
				continue;
			}
		}
		// Find matching variable
		for (i = 0; i < config->symbols->length; ++i) {
			struct sway_variable *var = config->symbols->items[i];
			int vnlen = strlen(var->name);
			if (strncmp(find, var->name, vnlen) == 0) {
				int vvlen = strlen(var->value);
				char *newstr = malloc(strlen(str) - vnlen + vvlen + 1);
				if (!newstr) {
					sway_log(L_ERROR,
						"Unable to allocate replacement "
						"during variable expansion");
					break;
				}
				char *newptr = newstr;
				int offset = find - str;
				strncpy(newptr, str, offset);
				newptr += offset;
				strncpy(newptr, var->value, vvlen);
				newptr += vvlen;
				strcpy(newptr, find + vnlen);
				free(str);
				str = newstr;
				find = str + offset + vvlen;
				break;
			}
		}
		if (i == config->symbols->length) {
			++find;
		}
	}
	return str;
}
