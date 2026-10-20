/* glsl_es_test: translate dumped game shaders to ES 3.20 for offline
 * validation with glslang. Usage: glsl_es_test in.glsl out.{vert,frag} */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glsl_es.h"

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    char *src, *es;
    int frag;
    if (argc < 3 || !(f = fopen(argv[1], "rb"))) return 2;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    src = calloc(1, (size_t)n + 1);
    fread(src, 1, (size_t)n, f);
    fclose(f);
    frag = strstr(argv[2], ".frag") != NULL;
    es = glsl_es_translate(src, frag, NULL);
    if (!(f = fopen(argv[2], "wb"))) return 2;
    fputs(es, f);
    fclose(f);
    return 0;
}
