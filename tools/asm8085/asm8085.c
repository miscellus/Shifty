#include "asm8085.h"

char startdir[PATH_MAX]; // this stores the directory in which the command is run

void help() {
    
    printf("asm8085 v" VERSION " (build " BUILD ")\n\n");
    printf("usage: asm8085 -h | [-o output] [-l file] [-d file] [-c] source\n");
    printf("\t-h ............ Show help\n");
    printf("\t-o <file> ..... Set output file\n");
    printf("\t-l <file> ..... Write listing\n");
    printf("\t-d <file> ..... Write debug info\n");
    printf("\t-c ............ Output CO file for NEC PC-8201A\n");
    
    exit(0);
}

// Replace extension by '.bin'
char *change_extension(const char *fname, const char *new_ext) {
    char *bin, *dot, *slash; 
    bin = copy_string(fname);
    bin = realloc(bin, strlen(bin)+strlen(new_ext)+1); // make sure there is room

    // Find last slash and dot
    slash = strrchr(bin, '/');
    dot = strrchr(bin, '.');
    
    if (dot == NULL || slash > dot) {
        // No dot, or slash after dot: append new_ext
        strcat(bin, new_ext);
    } else {
        // Dot, replace extension by new_ext
        strcpy(dot, new_ext);
    }
    
    return bin;
}

int main(int argc, char **argv) {
    int c;
    char *inp=NULL, *outp=NULL, *list=NULL, *debug_info=NULL;
    unsigned char *mem;
    FILE *outf, *listf, *debug_info_f;
    size_t outsize;
    int co_file = 0;
    
    // Allocate 64K for binary output
    if ((mem = malloc(65536)) == NULL) {
        fprintf(stderr, "memory allocation failure.\n");
        exit(1);
    }
    
    // Store current directory
    if (getcwd(startdir, PATH_MAX) == NULL) {
        fprintf(stderr, "cannot get wd: %s\n", strerror(errno));
        exit(1);
    }
    
    // Handle arguments
    while((c = getopt(argc, argv, "ho:l:d:c")) != -1) {
        switch(c) {
            case '?':
                if (optopt == 'o' || optopt == 'l') {
                    fprintf(stderr, "-%c requires an argument.\n", optopt);
                } else {
                    fprintf(stderr, "Unknown option: -%c\n", optopt);
                }
                
                exit(1);
            
            case 'h': help(); break;
            case 'o': outp = optarg; break;
            case 'l': list = optarg; break;
            case 'd': debug_info = optarg; break;
            case 'c': co_file = 1; break;
        }
    }
    
    if (optind != argc-1) {
        fprintf(stderr, "asm8085: no source file given\n");
        exit(1);
    }
    
    inp = argv[optind];
    
    // If no output file is given, change the input extension
    if (outp == NULL) outp = change_extension(inp, co_file ? ".co" : ".bin");
    
    // Try to assemble the file. 
    struct asmstate *state = init_asmstate();
    
    struct line *lines = assemble(state, inp);
    if (lines == NULL) exit(1);
    
    if (!complete(state, lines)) exit(2);
    
    // Restore the old working directory
    if (chdir(startdir) == -1) {
        fprintf(stderr, "cannot restore wd: %s\n", strerror(errno));
        exit(1);
    }
    
    // Write the binary file
    if (!strcmp(outp, "-")) {
        outf = stdout;  // allow output to STDOUT
    } else if ((outf = fopen(outp, "wb")) == NULL) {
        fprintf(stderr, "cannot open %s for writing: %s\n", outp, strerror(errno));
        exit(1);
    }
    
    outsize = make_binary(lines, mem);

    printf("Binary Size: % 20d - %# 15x\n", outsize, outsize);

    if (co_file) write_co_file_header(lines, outsize, outf);

    if (fwrite(mem, 1, outsize, outf) != outsize) {
        fprintf(stderr, "write error: %s\n", strerror(errno));
        exit(1);
    }
    
    if (outf != stdout) fclose(outf);
    
    // Write the listing if the user wanted one
    if (list != NULL) {
        if (!strcmp(list, "-")) {
            listf = stdout; // allow listing output to stdout
        } else if ((listf = fopen(list, "wb")) == NULL) {
            fprintf(stderr, "cannot open %s for writing: %s\n", list, strerror(errno));
            exit(1);
        }
        write_listing(listf, state, lines);
        if (listf != stdout) fclose(listf);
    }

    if (debug_info != NULL) {
        if (strcmp(debug_info, "-") == 0) {
            debug_info_f = stdout;
        }
        else if ((debug_info_f = fopen(debug_info, "wb")) == NULL) {
            fprintf(stderr, "cannot open %s for writing: %s\n", debug_info, strerror(errno));
            exit(1);
        }
        write_json_debugger_file(debug_info_f, state, lines);
        if (debug_info_f != stdout) fclose(debug_info_f);
    }
    
    return 0;
    
}