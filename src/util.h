/* See LICENSE file for copyright and license details. */

#define MAX(A, B)        ((A) > (B) ? (A) : (B))
#define MIN(A, B)        ((A) < (B) ? (A) : (B))
#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))

#ifdef _DEBUG
#define DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG(...)
#endif

void die(const char *fmt, ...);
void *ecalloc(size_t nmemb, size_t size);
int normalizepath(const char *path, char **normal);
int mkdirp(const char *path);
int parentdir(const char *path, char **parent);
/* $XDG_CONFIG_HOME/ewm/<name>, else $HOME/.config/ewm/<name>; -1 if unset
 * or truncated */
int configpath(char *buf, size_t len, const char *name);
