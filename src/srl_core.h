/* srl_core.h - find, edit and patch *_SRL globals in an ELF binary. */
#ifndef SRL_CORE_H
#define SRL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SRL_SUFFIX "_SRL"

typedef enum {
    SRL_K_INT,   /* signed integer (or enum)            */
    SRL_K_UINT,  /* unsigned integer                    */
    SRL_K_BOOL,  /* _Bool / bool                        */
    SRL_K_FLOAT, /* float (4) / double (8)              */
    SRL_K_PTR,   /* pointer, shown as hex address       */
    SRL_K_STR,   /* char[N], edited as a C-ish string   */
    SRL_K_HEX,   /* anything else: raw bytes in hex     */
} srl_kind;

typedef struct {
    char *name;           /* the variable being edited (pointee)   */
    char *srl_name;       /* the NAME_SRL[_TAG] pointer that marks it */
    char *tag;            /* "INT", "FLOAT", "STR"... or NULL if untagged */
    uint64_t addr;
    size_t size;
    char *section;
    long long file_off;   /* offset in file, -1 for NOBITS (.bss) */
    char *type_disp;      /* type as shown to the user            */
    srl_kind kind;
    size_t elem_size;     /* size of one element                  */
    size_t count;         /* number of elements (arrays)          */
    unsigned char *orig;  /* bytes as found in the file           */
    unsigned char *cur;   /* bytes after the user's edits         */

    /* optional NAME_SRL_RANGE_MIN / NAME_SRL_RANGE_MAX globals (INT/UINT/FLOAT only) */
    bool has_min, has_max;
    unsigned char rmin_raw[8], rmax_raw[8];
    size_t rmin_size, rmax_size;
    int64_t min_i, max_i;   /* SRL_K_INT   */
    uint64_t min_u, max_u;  /* SRL_K_UINT  */
    double min_f, max_f;    /* SRL_K_FLOAT */
} srl_var;

typedef struct {
    char *path;
    unsigned char *data;  /* whole file, never modified */
    size_t len;
    srl_var *vars;
    size_t nvars;
    bool have_types;      /* every variable has a known tag */
    size_t nunresolved;   /* _SRL symbols whose target wasn't found */
    bool pie;             /* ET_DYN: loaded at a random base          */
    uint64_t load_vaddr;  /* page-aligned vaddr of the PT_LOAD at file offset 0 */
} srl_binary;

srl_binary *srl_load(const char *path, char *err, size_t errlen);
void srl_free(srl_binary *b);

bool srl_var_editable(const srl_var *v);
bool srl_var_modified(const srl_var *v);
size_t srl_count_modified(const srl_binary *b);
void srl_revert(srl_binary *b);

/* Render v->cur (or any v->size bytes) as text. Returns malloc'd string. */
char *srl_format(const srl_var *v);
char *srl_format_bytes(const srl_var *v, const unsigned char *bytes);
/* Parse text into out (v->size bytes), enforcing the variable's range.
 * On failure fills err, returns false. */
bool srl_parse(const srl_var *v, const char *text, unsigned char *out,
               char *err, size_t errlen);

/* Build the patched image in a sealed memfd (nothing touches the disk).
 * Returns the fd or -1. */
int srl_make_memfd(const srl_binary *b, char *err, size_t errlen);

/* Write the patched binary to path (same permissions as the original).
 * Refuses to overwrite the original file. */
bool srl_save(const srl_binary *b, const char *path, char *err, size_t errlen);

/* fork + fexecve(exe_fd). If tty_fd >= 0 the child gets it as its
 * controlling terminal and stdin/stdout/stderr. Returns child pid or -1. */
pid_t srl_spawn(int exe_fd, int tty_fd, char *const argv[],
                char *const envp[], const char *cwd);

/* ---- runtime editing of a running child ------------------------------
 * Variables are read/written straight in the child's memory with
 * process_vm_readv/writev (a parent may do this to its own child). */
typedef struct {
    pid_t pid;
    unsigned long ino;    /* inode of the memfd the child was exec'd from */
    bool ready;           /* load address found (the exec has happened)  */
    uint64_t bias;        /* runtime address = bias + st_value            */
} srl_live;

/* Call right after srl_spawn, before closing exe_fd. */
void srl_live_init(srl_live *L, pid_t pid, int exe_fd);
/* Find the load address from /proc/PID/maps. Cheap once ready. */
bool srl_live_locate(srl_live *L, const srl_binary *b, char *err, size_t errlen);
bool srl_live_read(srl_live *L, const srl_binary *b, const srl_var *v,
                   unsigned char *out, char *err, size_t errlen);
bool srl_live_write(srl_live *L, const srl_binary *b, const srl_var *v,
                    const unsigned char *bytes, char *err, size_t errlen);

#endif
