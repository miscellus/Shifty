#include "listing.h"

// Write up to four bytes
void write_bytes(FILE *f, const struct line *l, int offset, int first) {
    const char *byte_out[] = { 
        "           ", // no bytes
        "%02X         ", // one byte
        "%02X %02X      ", // two bytes
        "%02X %02X %02X   ", // three bytes 
        "%02X %02X %02X %02X" // four bytes 
    }; 
    
    int n = l->n_bytes - offset;
    
    
    if (!first) fprintf(f,"            ");
    if (n<0) FATAL_ERROR("negative byte amount");
    
    const unsigned char *b = l->bytes + offset; 
    
    switch(n) {
        case 0: // No bytes
            fprintf(f, byte_out[0]);
            break;
        case 1:
            fprintf(f, byte_out[1], b[0]);
            break;
        case 2:
            fprintf(f, byte_out[2], b[0], b[1]);
            break;
        case 3:
            fprintf(f, byte_out[3], b[0], b[1], b[2]);
            break;
        default:    // 4 or up
            fprintf(f, byte_out[4], b[0], b[1], b[2], b[3]);
    }
    
    if (!first) fprintf(f, "\n");
}

void write_listing(FILE *f, const struct asmstate *state, const struct line *lines) {
    const struct line *line;
    int offset;
    intptr_t value = 0;
    
    // Handle all the lines
    for (line=lines; line!=NULL; line=line->next_line) {
        // Print line number, if it isn't auto-generated
        if (line->info.lineno == 0) fprintf(f, "      ");
        else fprintf(f, "%5d ", line->info.lineno);
        
        // If the line defines bytes, print the location 
        if (line->n_bytes > 0) fprintf(f, "%04X: ", line->location);
        // If it is an 'equ', print its value
        else if (line->instr.type == DIRECTIVE 
              && line->instr.instr == DIR_equ) {
         
            set_base(state->knowns, line->info.lastlabel);
            if (!get_var(state->knowns, line->label, &value)) {
                fprintf(f, "???? =");
            } else {
                fprintf(f, "%04X =", (unsigned short) value);
            }                
        } else fprintf(f, "      ");
        
        // For a binary include, don't print all the bytes
        if (line->instr.type == DIRECTIVE
         && line->instr.instr == DIR_incbin) {
             fprintf(f, "[.........] %s\n", line->raw_text);
        } else {
            // Print bytes, if there are any
            offset = 0;
            write_bytes(f, line, offset, TRUE);
        
            // Print rest of line
            fprintf(f, " %s\n", line->raw_text);
        
            // If there were more than 4 bytes, print the rest of the bytes on separate lines
            for (offset = 4; offset < line->n_bytes; offset += 4)
                write_bytes(f, line, offset, FALSE);
        }
    }
    
    // If there are no symbols defined, skip the symbol table
    if (state->knowns->variables == NULL) return; 
    
    // Then write the symbol table 
    fprintf(f, "\n\n");
    fprintf(f, "************************************************************\n");
    fprintf(f, "                        Symbol table                        \n");
    fprintf(f, "************************************************************\n");
    fprintf(f, "\n\n");
    
    fprintf(f, "Name                    = Value\n");
    fprintf(f, "-----------------------   ----------------------------------\n");
    
    struct variable *v;
    // They are in reverse order of occurrence, so find the first one
    for (v = state->knowns->variables; v != NULL && v->next != NULL; v = v->next);
    
    // And then print them in reverse order
    for (; v != NULL; v = v->prev) {
        fprintf(f, "%-23s = %04Xh\n", v->name, (unsigned short) v->value);
    }
}
// Helper function to safely print strings with JSON-escaped characters
static void print_json_string(FILE *f, const char *str) {
    if (!str) {
        fprintf(f, "\"\"");
        return;
    }
    fprintf(f, "\"");
    while (*str) {
        switch (*str) {
            case '\\': fprintf(f, "\\\\"); break;
            case '"':  fprintf(f, "\\\""); break;
            case '\n': fprintf(f, "\\n");  break;
            case '\r': fprintf(f, "\\r");  break;
            case '\t': fprintf(f, "\\t");  break;
            default:   fputc(*str, f);     break;
        }
        str++;
    }
    fprintf(f, "\"");
}

// Helper to stream an entire file from disk straight into an escaped JSON string
static void embed_source_file(FILE *json_f, const char *filepath) {
    FILE *source_f = fopen(filepath, "r");
    if (!source_f) {
        // If the file can't be opened, output an empty string or error message
        fprintf(json_f, "\" ; ERROR: Could not embed source file on assembly host \"");
        return;
    }

    fprintf(json_f, "\"");
    int ch;
    while ((ch = fgetc(source_f)) != EOF) {
        switch (ch) {
            case '\\': fprintf(json_f, "\\\\"); break;
            case '"':  fprintf(json_f, "\\\""); break;
            case '\n': fprintf(json_f, "\\n");  break;
            case '\r': fprintf(json_f, "\\r");  break;
            case '\t': fprintf(json_f, "\\t");  break;
            default:   fputc(ch, json_f);       break;
        }
    }
    fprintf(json_f, "\"");

    fclose(source_f);
}

void write_json_debugger_file(FILE *f, const struct asmstate *state, const struct line *lines) {
    const struct line *line;
    struct variable *v;
    int first_item, i, j;

    // Array to track unique filenames
    const char *unique_files[1024];
    int num_files = 0;

    // Pre-pass: Gather unique filenames
    for (line = lines; line != NULL; line = line->next_line) {
        if (line->info.lineno == 0 || line->info.filename == NULL) continue;

        int found = 0;
        for (i = 0; i < num_files; i++) {
            if (strcmp(unique_files[i], line->info.filename) == 0) {
                found = 1;
                break;
            }
        }

        if (!found && num_files < 256) {
            unique_files[num_files++] = line->info.filename;
        }
    }

    fprintf(f, "{\n");

    // Output safe "files" array
    fprintf(f, "  \"files\": [\n");
    for (i = 0; i < num_files; i++) {
        fprintf(f, "    ");
        print_json_string(f, unique_files[i]);
        fprintf(f, "%s\n", (i == num_files - 1) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"file_contents\": [\n");
    for (i = 0; i < num_files; i++) {
        fprintf(f, "    ");
        embed_source_file(f, unique_files[i]);
        fprintf(f, "%s\n", (i == num_files - 1) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    // Output "lines" array
    fprintf(f, "  \"lines\": [\n");
    first_item = 1;

    for (line = lines; line != NULL; line = line->next_line) {
        if (line->info.lineno == 0 || line->info.filename == NULL) continue;

        // Locate the file index
        int file_id = 0;
        for (j = 0; j < num_files; j++) {
            if (strcmp(unique_files[j], line->info.filename) == 0) {
                file_id = j;
                break;
            }
        }

        if (!first_item) fprintf(f, ",\n");
        first_item = 0;

        fprintf(f, "    {\n");
        fprintf(f, "      \"file_id\": %d,\n", file_id);
        fprintf(f, "      \"line\": %d,\n", line->info.lineno);

        // Map every byte address occupied by this instruction
        fprintf(f, "      \"addresses\": [");
        for (i = 0; i < line->n_bytes; i++) {
            fprintf(f, "%d%s", line->location + i, (i == line->n_bytes - 1) ? "" : ", ");
        }
        fprintf(f, "],\n");

        // Dump raw bytes generated by this line
        fprintf(f, "      \"bytes\": [");
        if (line->instr.type == DIRECTIVE && line->instr.instr == DIR_incbin) {
            // Keep arrays empty for raw binary inclusions to preserve file size
        } else {
            for (i = 0; i < line->n_bytes; i++) {
                fprintf(f, "%d%s", line->bytes[i], (i == line->n_bytes - 1) ? "" : ", ");
            }
        }
        fprintf(f, "]\n");
        fprintf(f, "    }");
    }
    fprintf(f, "\n  ],\n");

    // Output "symbols" dictionary
    fprintf(f, "  \"symbols\": {\n");
    first_item = 1;

    // Reverse lookup to maintain structural order matching your list style
    for (v = state->knowns->variables; v != NULL && v->next != NULL; v = v->next);

    for (; v != NULL; v = v->prev) {
        if (!first_item) fprintf(f, ",\n");
        first_item = 0;

        fprintf(f, "    ");
        print_json_string(f, v->name);
        fprintf(f, ": %d", (unsigned short)v->value);
    }
    fprintf(f, "\n  }\n");

    fprintf(f, "}\n");
}
