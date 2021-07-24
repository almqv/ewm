/* See LICENSE file for copyright and license details. */

#include <stdio.h>
#include <stdarg.h>

#include "config.h"
#include "log.h" 

void flog(char* str) {
	FILE* fp;
	fp = fopen(logfile, "a");

	fprintf(fp, str);
	
	fclose(fp);
}
