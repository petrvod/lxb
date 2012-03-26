#include <R.h>
#include <Rinternals.h>
#include <stdint.h>
#include <stdbool.h>
#include "map_lib.h"

// strsep() does not exist on Windows, so include the version from libgw32c
#include "strsep.h"

// Max number of parameters in LXB file that we handle
#define MAX_PAR       99
// Max chars needed to print MAX_PAR (must be updated when MAX_PAR is!)
#define MAX_PAR_CHARS 2

typedef struct {
    int begin_text, end_text;
    int begin_data, end_data;
    int begin_analysis, end_analysis;
} fcs_header;

static int par_mask[MAX_PAR];


char *read_file(const char *filename, long *size)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    rewind(fp);
    if (!(filesize > 0)) {
        fclose(fp);
        return NULL;
    }

    char *buf = malloc(filesize);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    long actual = fread(buf, 1, filesize, fp);
    fclose(fp);

    if (actual != filesize) {
        free(buf);
        return NULL;
    }

    if (size)
        *size = filesize;

    return buf;
}

const char *parameter_key(int n, char type)
{
    // Key is of format "$PXY", where len(X) <= MAX_PAR_CHARS, and Y == type,
    // also include room for null terminator.
    static char buf[MAX_PAR_CHARS+4];
    if (n < 0 || n >= MAX_PAR)
        return "";

    sprintf(buf, "$P%d%c", n+1, type);
    return buf;
}

void init_parameter_mask(map_t txt)
{
    memset(par_mask, 0, MAX_PAR*sizeof(par_mask[0]));

    int npar = map_get_int(txt, "$PAR");
    for (int i = 0; i < npar; ++i) {
        const char *key = parameter_key(i, 'R');
        par_mask[i] = map_get_int(txt, key);
        if (par_mask[i] > 0)
            --par_mask[i];
    }
}

int parameter_mask(int n)
{
    return n >= 0 && n < MAX_PAR ? par_mask[n] : 0;
}

bool parse_header(const char *data, long size, fcs_header *hdr)
{
    if (!hdr) return false;

    if (size < 58) {
        warning("  Bad LXB: header data is too small (%lu)\n", size);
        return false;
    }

    if (0 != strncmp(data, "FCS3.0    ", 10)) {
        warning("  Bad LXB: magic bytes do not match\n");
        return false;
    }

    bool ok = true;
    ok &= sscanf(&data[10], "%8d", &hdr->begin_text);
    ok &= sscanf(&data[18], "%8d", &hdr->end_text);
    ok &= sscanf(&data[26], "%8d", &hdr->begin_data);
    ok &= sscanf(&data[34], "%8d", &hdr->end_data);
    ok &= sscanf(&data[42], "%8d", &hdr->begin_analysis);
    ok &= sscanf(&data[50], "%8d", &hdr->end_analysis);

    if (!ok)
        warning("  Bad LXB: failed to parse segment offsets\n");

    return ok;
}

char *dup2str(const void *buf, long size)
{
    char *str = (char *)malloc(size + 1);
    if (!str)
        return NULL;

    str[size] = 0;
    return memcpy(str, buf, size);
}

map_t parse_text(const char *text, long size)
{
    if (size < 2)
        return NULL;

    map_t m = map_create();
    char *sep = dup2str(text, 1);
    char *data = dup2str(text+1, size-1);

    char *p = data;
    for (;;) {
        // FIXME: FCS 3.0 allows the separator character to appear in keys and
        // values by repeating the separator twice -- this is currently NOT
        // handled.
        // For example, if sep='/' then "k//ey/value/" should be parsed as
        // "k/ey"="value", whereas we parse it as { "k"="", "ey"="value" }.
        char *key = strsep(&p, sep);
        if (!key) break;
        char *val = strsep(&p, sep);
        if (!val) break;

        map_set(m, key, val);
    }

    free(data);
    free(sep);

    return m;
}

bool check_par_format(map_t txt)
{
    int npar = map_get_int(txt, "$PAR");
    if (npar > MAX_PAR) {
        warning("  Unsupported LXB: too many parameters (%d)\n", npar);
        return false;
    }

    const char *data_type = map_get(txt, "$DATATYPE");
    if (strcasecmp("I", data_type) != 0) {
        warning("  Unsupported LXB: data is not integral "
                "($DATATYPE=%s)\n", data_type);
        return false;
    }

    const char *mode = map_get(txt, "$MODE");
    if (strcasecmp("L", mode) != 0) {
        warning("  Unsupported LXB: data not in list format "
                "($MODE=%s)\n", mode);
        return false;
    }

    const char *byteord = map_get(txt, "$BYTEORD");
    if (strcmp("1,2,3,4", byteord) != 0) {
        warning("  Unsupported LXB: data not in little endian format "
                "($BYTEORD=%s)\n", byteord);
        return false;
    }

    const char *unicode = map_get(txt, "$UNICODE");
    if (*unicode) {
        // FIXME: Support Unicode.  We try to parse the data even if the text
        // segment contains Unicode characters, so don't return false here.
        warning("  Unsupported LXB: Unicode flag detected,"
                " output may be corrupted\n");
    }

    init_parameter_mask(txt);

    for (int i = 0; i < npar; ++i) {
        const char *key = parameter_key(i, 'B');
        int bits = map_get_int(txt, key);
        if (bits != 32) {
            warning("  Unsupported LXB: parameter %d is not 32 bits "
                    "(%s=%d)\n", i, key, bits);
            return false;
        }
    }

    return true;
}

struct set_value_s {
    SEXP v;
    int  n;
};

static void set_value_f(const char *key, const char *value,
                        struct set_value_s *state)
{
    SET_STRING_ELT(state->v, (state->n)++, mkChar(value));
}

static void set_key_f(const char *key, const char *value,
                      struct set_value_s *state)
{
    // Remove initial dollar sign from keys since R uses these to index lists.
    if (key[0] == '$')
        ++key;

    SET_STRING_ELT(state->v, (state->n)++, mkChar(key));
}

void parse_segments(char *buf, long size, map_t *outTxt, char **outData)
{
    if (outTxt)  *outTxt = NULL;
    if (outData) *outData = NULL;

    fcs_header hdr;
    bool ok = parse_header(buf, size, &hdr);
    if (!ok) {
        warning("  Bad LXB: failed to parse header\n");
        return;
    }

    long txt_size = hdr.end_text - hdr.begin_text;
    if (!(txt_size > 0 && hdr.begin_text > 0 && hdr.end_text <= size)) {
        free(buf);
        warning("  Bad LXB: could not locate TEXT segment\n");
        return;
    }

    map_t txt = parse_text(buf + hdr.begin_text, txt_size);
    if (outTxt) *outTxt = txt;

    if (!check_par_format(txt))
        return;

    long data_size = hdr.end_data - hdr.begin_data;
    if (!(data_size > 0 && hdr.begin_data > 0 && hdr.end_data <= size)) {
        warning("  Bad LXB: could not locate DATA segment\n");
        return;
    }

    if (outData) *outData = buf + hdr.begin_data;
}

SEXP map_to_Rlist(map_t map)
{
    int len = map_length(map);

    SEXP vals;
    PROTECT(vals = allocVector(STRSXP, len));
    struct set_value_s seed = { vals, 0 };
    map_fold(map, (fold_func_t)set_value_f, (void*)&seed);

    SEXP names;
    PROTECT(names = allocVector(STRSXP, len));
    seed.v = names;
    seed.n = 0;
    map_fold(map, (fold_func_t)set_key_f, (void*)&seed);

    namesgets(vals, names);

    return vals;
}

SEXP read_lxb(SEXP inFilename, SEXP inTextFlag)
{
    const char *filename = CHAR(STRING_ELT(inFilename, 0));
    int textFlag = *LOGICAL(inTextFlag);

    long size;
    char *buf = read_file(filename, &size);
    if (!buf) {
        warning("  Could not read file: %s\n", filename);
        return R_NilValue;
    }

    map_t txt;      // alloc'ed by parse_segments(), must free()
    char *data;     // will point inside 'buf', do not free()
    parse_segments(buf, size, &txt, &data);
    if (!txt) {
        free(buf);
        return R_NilValue;
    }

    SEXP out, outnames;
    int outLen = textFlag ? 2 : 1;
    PROTECT(out = allocVector(VECSXP, outLen));
    PROTECT(outnames = allocVector(STRSXP, outLen));

    if (data) {
        // Allocate output matrix to be ntot columns times npar rows
        int npar = map_get_int(txt, "$PAR");
        int ntot = map_get_int(txt, "$TOT");
        SEXP mat;
        PROTECT(mat = allocMatrix(INTSXP, npar, ntot));

        // Initialize vector with row names (taken from $PxN parameter)
        SEXP rownames;
        PROTECT(rownames = allocVector(STRSXP, npar));
        for (int i = 0; i < npar; ++i) {
            const char *label = map_get(txt, parameter_key(i, 'N'));
            SET_STRING_ELT(rownames, i, mkChar(label));
        }

        // Set dimnames attribute on output matrix
        SEXP dimnames;
        PROTECT(dimnames = allocVector(VECSXP, 2));
        SET_VECTOR_ELT(dimnames, 0, rownames);
        dimnamesgets(mat, dimnames);

        // Set actual data in output matrix
        const int32_t *src = (int32_t*)data;
        int *dst           = INTEGER(mat);
        for (int i = 0; i < ntot*npar; ++i)
            *dst++ = (int)*src++ & parameter_mask(i%npar);

        SET_VECTOR_ELT(out, 0, mat);
        UNPROTECT(3);
    } else {
        SET_VECTOR_ELT(out, 0, R_NilValue);
    }

    SET_STRING_ELT(outnames, 0, mkChar("data"));

    if (textFlag) {
        SET_VECTOR_ELT(out, 1, map_to_Rlist(txt));
        SET_STRING_ELT(outnames, 1, mkChar("text"));
        UNPROTECT(2);
    }

    namesgets(out, outnames);

    UNPROTECT(2);
    if (txt)
        map_free(txt);
    free(buf);

    return out;
}
