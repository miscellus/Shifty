#include "bin_output.h"

// Given a list of assembled lines, make binary output. 
// It is assumed that the buffer is at least 64K big.
 
size_t make_binary(const struct line *lines, unsigned char *buf) {
    const struct line *line;
    size_t idx = 0;
    
    for (line = lines; line != NULL; line=line->next_line) {
        if (line->needs_process) FATAL_ERROR("unprocessed line was passed in");
        if (line->n_bytes == 0) continue;
        
        // this is a fatal error, because the assembler is supposed to catch this
        // beforehand and give a nicer error message.
        if (idx+line->n_bytes > (1<<16)) FATAL_ERROR("tried to output more than 64k");
        memcpy(buf+idx, line->bytes, line->n_bytes);
        idx += line->n_bytes;
    }
        
    return idx; 
}

void write_co_file_header(const struct line *line, size_t length, FILE *outf)
{
    while (line && line->instr.instr != DIR_org) {
        line = line->next_line;
    }

    int loc = 0;
    if (line) loc = line->location;

    struct {uint16_t org, len, entry;} header;
    header.org = loc;
    header.len = length;
    header.entry = loc;
    if (fwrite(&header, sizeof(header), 1, outf) != 1) {
        fprintf(stderr, "write error: %s\n", strerror(errno));
        fclose(outf);
        exit(1);
    }
}