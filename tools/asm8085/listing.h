#ifndef __LISTING_H__

#include <stdio.h>

#include "parser.h"
#include "assembler.h"

void write_listing(FILE *f, const struct asmstate *state, const struct line *lines);
void write_json_debugger_file(FILE *f, const struct asmstate *state, const struct line *lines);

#endif