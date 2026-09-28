/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

void *ecalloc(size_t nmemb, size_t size) {
	void *p;

	if (!(p = calloc(nmemb, size)))
		die("calloc:");
	return p;
}

void die(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	} else {
		fputc('\n', stderr);
	}

	exit(1);
}

int normalizepath(const char *path, char **normal) {
	const size_t len = strlen(path);
	char *out        = ecalloc(len + 1, sizeof(char));
	size_t n         = 0;

	for (const char *p = path; *p; p++) {
		// Collapse repeated slashes
		if (*p == '/' && n > 0 && out[n - 1] == '/')
			continue;
		out[n++] = *p;
	}

	// Strip a trailing slash, but keep the root directory "/"
	if (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = '\0';

	*normal = out;

	return 0;
}

int parentdir(const char *path, char **parent) {
	char *normal;
	char *walk;
	size_t len;

	*parent = NULL;
	normalizepath(path, &normal);

	// Pointer to last '/'
	if (!(walk = strrchr(normal, '/'))) {
		free(normal);
		return -1;
	}

	// Parent of "/x" is "/"
	len     = walk == normal ? 1 : (size_t) (walk - normal);
	*parent = ecalloc(len + 1, sizeof(char));
	memcpy(*parent, normal, len);

	free(normal);

	return 0;
}

int mkdirp(const char *path) {
	char *normal;
	char *p;
	char saved;
	struct stat s;
	int ret = 0;

	normalizepath(path, &normal);
	if (normal[0] == '\0') {
		free(normal);
		return 0;
	}

	// Create every path prefix ending before a '/' and finally the full path.
	// Start at index 1 so that the root "/" is never created.
	for (p = normal + 1;; p++) {
		if (*p != '/' && *p != '\0')
			continue;

		saved = *p;
		*p    = '\0';

		if (stat(normal, &s) == 0) {
			if (!S_ISDIR(s.st_mode)) {
				fprintf(stderr, "Not a directory: %s\n", normal);
				ret = -1;
			}
		} else if (errno != ENOENT) {
			fprintf(stderr, "Error statting directory %s: %s\n", normal,
			        strerror(errno));
			ret = -1;
		} else {
			DEBUG("Making directory %s\n", normal);
			if (mkdir(normal, 0700) < 0 && errno != EEXIST) {
				fprintf(stderr, "Failed to make directory %s: %s\n", normal,
				        strerror(errno));
				ret = -1;
			}
		}

		*p = saved;
		if (ret < 0 || saved == '\0')
			break;
	}

	free(normal);

	return ret;
}
