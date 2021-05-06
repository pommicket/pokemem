// stuff for dealing with various data types.

static DataType data_type_from_name(char const *name) {
	switch (name[0]) {
	case 'u':
		switch (atoi(&name[1])) {
		case 8:  return TYPE_U8;
		case 16: return TYPE_U16;
		case 32: return TYPE_U32;
		case 64: return TYPE_U64;
		}
		if (strncmp(name, "utf", 3) == 0) {
			switch (atoi(&name[3])) {
			case 16: return TYPE_UTF16;
			case 32: return TYPE_UTF32;
			}
		}
		break;
	case 's':
		switch (atoi(&name[1])) {
		case 8:  return TYPE_S8;
		case 16: return TYPE_S16;
		case 32: return TYPE_S32;
		case 64: return TYPE_S64;
		}
		break;
	case 'f':
		switch (atoi(&name[1])) {
		case 32: return TYPE_F32;
		case 64: return TYPE_F64;
		}
		break;
	case 'a':
		if (strcmp(name, "ascii") == 0)
			return TYPE_ASCII;
		break;
	}
	assert(0);
	return TYPE_U8;
}

static size_t data_type_size(DataType type) {
	switch (type) {
	case TYPE_U8:
	case TYPE_S8:
	case TYPE_ASCII:
		return 1;
	case TYPE_U16:
	case TYPE_S16:
	case TYPE_UTF16:
		return 2;
	case TYPE_U32:
	case TYPE_S32:
	case TYPE_F32:
	case TYPE_UTF32:
		return 4;
	case TYPE_U64:
	case TYPE_S64:
	case TYPE_F64:
		return 8;
	}
	return (size_t)-1;
}

// set str to "a" for 'a', "\\n" for '\n', "\\xff" for (wchar_t)255, etc.
static void char_to_str(uint32_t c, char *str, size_t str_size) {
	if (c <= WINT_MAX && iswgraph((wint_t)c)) {
		snprintf(str, str_size, "%lc", (wint_t)c);
	} else {
		switch (c) {
		case ' ': snprintf(str, str_size, "(space)"); break;
		case '\n': snprintf(str, str_size, "\\n"); break;
		case '\t': snprintf(str, str_size, "\\t"); break;
		case '\r': snprintf(str, str_size, "\\r"); break;
		case '\v': snprintf(str, str_size, "\\v"); break;
		case '\0': snprintf(str, str_size, "\\0"); break;
		default:
			if (c < 256)
				snprintf(str, str_size, "\\x%02x", (unsigned)c);
			else
				snprintf(str, str_size, "\\x%05lx", (unsigned long)c);
		}
	}
}

static bool char_from_str(char const *str, uint32_t *c) {
	if (str[0] == '\0') return false;
	if (str[0] == '\\') {
		switch (str[1]) {
		case 'n': *c = '\n'; return str[2] == '\0';
		case 't': *c = '\t'; return str[2] == '\0';
		case 'r': *c = '\r'; return str[2] == '\0';
		case 'v': *c = '\v'; return str[2] == '\0';
		case '0': *c = '\0'; return str[2] == '\0';
		case 'x': {
			unsigned long v = 0;
			int w = 0;
			if (sscanf(&str[2], "%lx%n", &v, &w) != 1 ||
				(size_t)w != strlen(&str[2]) || v > UINT32_MAX)
				return false;
			*c = (uint32_t)v;
			return true;
		}
		}
	}
	return unicode_utf8_to_utf32(c, str, strlen(str)) == strlen(str);
}

static void data_to_str(void const *value, DataType type, char *str, size_t str_size) {
	switch (type) {
	case TYPE_U8:  snprintf(str, str_size, "%" PRIu8,  *(uint8_t  *)value); break;
	case TYPE_U16: snprintf(str, str_size, "%" PRIu16, *(uint16_t *)value); break;
	case TYPE_U32: snprintf(str, str_size, "%" PRIu32, *(uint32_t *)value); break;
	case TYPE_U64: snprintf(str, str_size, "%" PRIu64, *(uint64_t *)value); break;
	case TYPE_S8:  snprintf(str, str_size, "%" PRId8,  *(int8_t  *)value);  break;
	case TYPE_S16: snprintf(str, str_size, "%" PRId16, *(int16_t *)value);  break;
	case TYPE_S32: snprintf(str, str_size, "%" PRId32, *(int32_t *)value);  break;
	case TYPE_S64: snprintf(str, str_size, "%" PRId64, *(int64_t *)value);  break;
	case TYPE_F32: snprintf(str, str_size, "%g", *(float  *)value);  break;
	case TYPE_F64: snprintf(str, str_size, "%g", *(double *)value);  break;
	case TYPE_UTF16: char_to_str(*(uint16_t *)value, str, str_size); break;
	case TYPE_UTF32: char_to_str(*(uint32_t *)value, str, str_size); break;
	case TYPE_ASCII:
		char_to_str((uint8_t)*(char *)value, str, str_size);
		break;
	
	}
}

// returns true on success, false if str is not a well-formatted value
static bool data_from_str(char const *str, DataType type, void *value) {
	int len = (int)strlen(str);
	int w = 0;
	uint32_t c = 0;
	switch (type) {
	case TYPE_U8:  return sscanf(str, "%" SCNu8  "%n", (uint8_t  *)value, &w) == 1 && w == len;
	case TYPE_S8:  return sscanf(str, "%" SCNd8  "%n", ( int8_t  *)value, &w) == 1 && w == len;
	case TYPE_U16: return sscanf(str, "%" SCNu16 "%n", (uint16_t *)value, &w) == 1 && w == len;
	case TYPE_S16: return sscanf(str, "%" SCNd16 "%n", ( int16_t *)value, &w) == 1 && w == len;
	case TYPE_U32: return sscanf(str, "%" SCNu32 "%n", (uint32_t *)value, &w) == 1 && w == len;
	case TYPE_S32: return sscanf(str, "%" SCNd32 "%n", ( int32_t *)value, &w) == 1 && w == len;
	case TYPE_U64: return sscanf(str, "%" SCNu64 "%n", (uint64_t *)value, &w) == 1 && w == len;
	case TYPE_S64: return sscanf(str, "%" SCNd64 "%n", ( int64_t *)value, &w) == 1 && w == len;
	case TYPE_F32: return sscanf(str, "%f"       "%n", (float    *)value, &w) == 1 && w == len;
	case TYPE_F64: return sscanf(str, "%lf"      "%n", (double   *)value, &w) == 1 && w == len;
	case TYPE_ASCII:
		if (!char_from_str(str, &c)) return false;
		if (c > 127) return false;
		*(uint8_t *)value = (uint8_t)c;
		return true;
	case TYPE_UTF16:
		if (!char_from_str(str, &c)) return false;
		if (c > 65535) return false;
		*(uint16_t *)value = (uint16_t)c;
		return true;
	case TYPE_UTF32:
		if (!char_from_str(str, &c)) return false;
		*(uint32_t *)value = c;
		return true;
	}
	assert(0);
	return false;
}

static bool data_equal(DataType type, void const *a, void const *b) {
	return memcmp(a, b, data_type_size(type)) == 0;
}
