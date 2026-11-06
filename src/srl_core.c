/* srl_core.c - ELF parsing, value codecs, in-memory patching and exec. */
#define _GNU_SOURCE
#include "srl_core.h"

#include <ctype.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static bool in_range(size_t len, uint64_t off, uint64_t n)
{
    return off <= len && n <= len - off;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p)
        abort();
    return p;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        abort();
    return p;
}

/* Marker names are <var>_SRL (type comes from debug info) or <var>_SRL_<TAG>
 * (type comes from the tag, e.g. _SRL_INT). Returns the length of <var>, or 0
 * when name is not a marker. *tag is set to the tag, or "" when untagged. */
static size_t marker_base_len(const char *name, const char **tag)
{
    const char *hit = NULL;
    for (const char *p = name; (p = strstr(p, SRL_SUFFIX)); p++)
        hit = p;
    if (!hit || hit == name)
        return 0;
    const char *rest = hit + strlen(SRL_SUFFIX);
    if (*rest == 0) {
        *tag = "";
        return (size_t)(hit - name);
    }
    if (rest[0] != '_' || !rest[1])
        return 0;
    for (const char *q = rest + 1; *q; q++)
        if (!isupper((unsigned char)*q) && !isdigit((unsigned char)*q))
            return 0;
    *tag = rest + 1;
    return (size_t)(hit - name);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/* ELF                                                                 */

typedef struct {
    const char *name;
    uint32_t type;
    uint64_t addr, off, size;
    uint32_t link;
} sec_t;

static const char *str_at(const srl_binary *b, const sec_t *s, uint64_t idx)
{
    if (s->type == SHT_NOBITS || idx >= s->size || !in_range(b->len, s->off, s->size))
        return NULL;
    const char *p = (const char *)b->data + s->off + idx;
    return memchr(p, 0, s->size - idx) ? p : NULL;
}

typedef struct {
    const char *name;
    uint64_t value, size;
    unsigned shndx;
    unsigned char type;
} sym_t;

static bool read_sym(const srl_binary *b, const sec_t *symsec, size_t j, bool is64, uint32_t *name, sym_t *out)
{
    size_t esz = is64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
    if (!in_range(b->len, symsec->off + j * esz, esz))
        return false;
    const unsigned char *p = b->data + symsec->off + j * esz;
    memset(out, 0, sizeof *out);
    if (is64) {
        Elf64_Sym s; memcpy(&s, p, sizeof s);
        *name = s.st_name; out->type = ELF64_ST_TYPE(s.st_info); out->shndx = s.st_shndx;
        out->value = s.st_value; out->size = s.st_size;
    } else {
        Elf32_Sym s; memcpy(&s, p, sizeof s);
        *name = s.st_name; out->type = ELF32_ST_TYPE(s.st_info); out->shndx = s.st_shndx;
        out->value = s.st_value; out->size = s.st_size;
    }
    return true;
}

/* file offset of [addr, addr+size) inside section sec, or -1 */
static long long file_offset(const srl_binary *b, const sec_t *sec, unsigned etype, uint64_t addr, uint64_t size)
{
    if (sec->type == SHT_NOBITS)
        return -1;
    uint64_t rel;
    if (etype == ET_REL)
        rel = addr;
    else if (addr >= sec->addr && addr - sec->addr < sec->size)
        rel = addr - sec->addr;
    else
        return -1;
    if (!in_range(b->len, sec->off + rel, size))
        return -1;
    return (long long)(sec->off + rel);
}

/* Where does pointer symbol ps point? Uses the dynamic relocation for it
 * (PIE: R_*_RELATIVE with addend = target, or symbol + addend), else the
 * address stored in the file (non-PIE, REL/RELR in-place addends). */
static bool resolve_pointer(const srl_binary *b, const sec_t *secs, unsigned shnum, bool is64,
                            unsigned etype, const sym_t *ps, uint64_t *target)
{
    long long poff = file_offset(b, &secs[ps->shndx], etype, ps->value, ps->size);
    uint64_t inplace = 0;
    bool have_inplace = false;
    if (poff >= 0) {
        if (is64) { uint64_t x; memcpy(&x, b->data + poff, 8); inplace = x; }
        else { uint32_t x; memcpy(&x, b->data + poff, 4); inplace = x; }
        have_inplace = true;
    }

    for (unsigned si = 0; si < shnum; si++) {
        const sec_t *rs = &secs[si];
        bool rela = rs->type == SHT_RELA;
        if (!rela && rs->type != SHT_REL)
            continue;
        size_t esz = is64 ? (rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel))
                          : (rela ? sizeof(Elf32_Rela) : sizeof(Elf32_Rel));
        if (!in_range(b->len, rs->off, rs->size))
            continue;
        for (size_t j = 0; j < rs->size / esz; j++) {
            const unsigned char *p = b->data + rs->off + j * esz;
            uint64_t off, info, symi;
            int64_t addend = 0;
            if (is64) {
                Elf64_Rela r; memset(&r, 0, sizeof r); memcpy(&r, p, esz);
                off = r.r_offset; info = r.r_info; addend = rela ? r.r_addend : 0;
                symi = ELF64_R_SYM(info);
            } else {
                Elf32_Rela r; memset(&r, 0, sizeof r); memcpy(&r, p, esz);
                off = r.r_offset; info = r.r_info; addend = rela ? r.r_addend : 0;
                symi = ELF32_R_SYM(info);
            }
            if (off != ps->value)
                continue;
            uint64_t A = rela ? (uint64_t)addend : inplace;
            if (symi == 0) { /* RELATIVE: base + A, base is 0 at link time */
                *target = A;
                return true;
            }
            if (rs->link >= shnum)
                return false;
            sym_t s;
            uint32_t nm;
            if (!read_sym(b, &secs[rs->link], symi, is64, &nm, &s) || s.shndx == SHN_UNDEF)
                return false;
            *target = s.value + A;
            return true;
        }
    }
    if (have_inplace && inplace) {
        *target = inplace;
        return true;
    }
    return false;
}

static int cmp_var(const void *a, const void *b)
{
    return strcmp(((const srl_var *)a)->name, ((const srl_var *)b)->name);
}

/* Where the image is mapped: the PT_LOAD that starts at file offset 0. */
static void read_phdrs(srl_binary *b, bool is64, unsigned etype)
{
    uint64_t phoff;
    unsigned phnum, phentsize;
    if (is64) {
        Elf64_Ehdr eh; memcpy(&eh, b->data, sizeof eh);
        phoff = eh.e_phoff; phnum = eh.e_phnum; phentsize = eh.e_phentsize;
    } else {
        Elf32_Ehdr eh; memcpy(&eh, b->data, sizeof eh);
        phoff = eh.e_phoff; phnum = eh.e_phnum; phentsize = eh.e_phentsize;
    }
    b->pie = etype == ET_DYN;
    b->load_vaddr = 0;
    size_t need = is64 ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr);
    if (phentsize < need || !in_range(b->len, phoff, (uint64_t)phnum * phentsize))
        return;
    bool found = false;
    uint64_t best_off = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *p = b->data + phoff + (uint64_t)i * phentsize;
        uint64_t type, off, vaddr;
        if (is64) {
            Elf64_Phdr ph; memcpy(&ph, p, sizeof ph);
            type = ph.p_type; off = ph.p_offset; vaddr = ph.p_vaddr;
        } else {
            Elf32_Phdr ph; memcpy(&ph, p, sizeof ph);
            type = ph.p_type; off = ph.p_offset; vaddr = ph.p_vaddr;
        }
        if (type != PT_LOAD || (found && off >= best_off))
            continue;
        uint64_t pg = 4096; /* the kernel maps segments page by page */
        found = true;
        best_off = off;
        b->load_vaddr = (vaddr - (off & (pg - 1))) & ~(pg - 1);
    }
}

static const sym_t *find_sym(const sym_t *syms, size_t n, const char *name)
{
    for (size_t k = 0; k < n; k++)
        if (!strcmp(syms[k].name, name))
            return &syms[k];
    return NULL;
}

/* Raw bytes of a NAME_SRL_RANGE_MIN/MAX global (zeros if it lives in .bss). */
static bool read_range_sym(const srl_binary *b, const sec_t *secs, unsigned etype, const sym_t *rs,
                           unsigned char out[8], size_t *size)
{
    if (!rs || rs->size == 0 || rs->size > 8)
        return false;
    const sec_t *sec = &secs[rs->shndx];
    memset(out, 0, 8);
    if (sec->type != SHT_NOBITS) {
        long long off = file_offset(b, sec, etype, rs->value, rs->size);
        if (off < 0)
            return false;
        memcpy(out, b->data + off, rs->size);
    }
    *size = rs->size;
    return true;
}

static bool parse_elf(srl_binary *b, char *err, size_t errlen)
{
    const unsigned char *d = b->data;
    if (b->len < EI_NIDENT || memcmp(d, ELFMAG, SELFMAG) != 0) {
        seterr(err, errlen, "not an ELF file");
        return false;
    }
    const uint16_t one = 1;
    int host = *(const unsigned char *)&one ? ELFDATA2LSB : ELFDATA2MSB;
    if (d[EI_DATA] != host) {
        seterr(err, errlen, "ELF byte order differs from this machine (unsupported)");
        return false;
    }
    bool is64 = d[EI_CLASS] == ELFCLASS64;
    if (!is64 && d[EI_CLASS] != ELFCLASS32) {
        seterr(err, errlen, "unknown ELF class");
        return false;
    }

    uint64_t shoff;
    unsigned shnum, shentsize, shstrndx, etype;
    if (is64) {
        Elf64_Ehdr eh;
        if (b->len < sizeof eh) goto trunc;
        memcpy(&eh, d, sizeof eh);
        shoff = eh.e_shoff; shnum = eh.e_shnum; shentsize = eh.e_shentsize;
        shstrndx = eh.e_shstrndx; etype = eh.e_type;
    } else {
        Elf32_Ehdr eh;
        if (b->len < sizeof eh) goto trunc;
        memcpy(&eh, d, sizeof eh);
        shoff = eh.e_shoff; shnum = eh.e_shnum; shentsize = eh.e_shentsize;
        shstrndx = eh.e_shstrndx; etype = eh.e_type;
    }
    read_phdrs(b, is64, etype);

    size_t shdr_sz = is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    if (shoff == 0 || shnum == 0) {
        seterr(err, errlen, "binary has no section headers");
        return false;
    }
    if (shentsize < shdr_sz || !in_range(b->len, shoff, (uint64_t)shnum * shentsize))
        goto trunc;

    sec_t *secs = calloc(shnum, sizeof *secs);
    if (!secs) abort();
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *p = d + shoff + (uint64_t)i * shentsize;
        uint32_t nm;
        if (is64) {
            Elf64_Shdr sh; memcpy(&sh, p, sizeof sh);
            nm = sh.sh_name; secs[i].type = sh.sh_type; secs[i].addr = sh.sh_addr;
            secs[i].off = sh.sh_offset; secs[i].size = sh.sh_size; secs[i].link = sh.sh_link;
        } else {
            Elf32_Shdr sh; memcpy(&sh, p, sizeof sh);
            nm = sh.sh_name; secs[i].type = sh.sh_type; secs[i].addr = sh.sh_addr;
            secs[i].off = sh.sh_offset; secs[i].size = sh.sh_size; secs[i].link = sh.sh_link;
        }
        secs[i].name = (const char *)(uintptr_t)nm; /* resolved below */
    }
    for (unsigned i = 0; i < shnum; i++) {
        uint32_t nm = (uint32_t)(uintptr_t)secs[i].name;
        const char *s = shstrndx < shnum ? str_at(b, &secs[shstrndx], nm) : NULL;
        secs[i].name = s ? s : "?";
    }

    /* pass 1: every defined object symbol (symtab + dynsym) */
    sym_t *syms = NULL;
    size_t nsyms = 0, symcap = 0;
    for (unsigned si = 0; si < shnum; si++) {
        const sec_t *ss = &secs[si];
        if (ss->type != SHT_SYMTAB && ss->type != SHT_DYNSYM)
            continue;
        if (ss->link >= shnum || !in_range(b->len, ss->off, ss->size))
            continue;
        const sec_t *strs = &secs[ss->link];
        size_t esz = is64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
        size_t n = ss->size / esz;
        for (size_t j = 0; j < n; j++) {
            sym_t sy;
            uint32_t name;
            if (!read_sym(b, ss, j, is64, &name, &sy))
                continue;
            if (sy.type != STT_OBJECT || sy.size == 0)
                continue;
            if (sy.shndx == SHN_UNDEF || sy.shndx >= SHN_LORESERVE || sy.shndx >= shnum)
                continue;
            sy.name = str_at(b, strs, name);
            if (!sy.name || !*sy.name)
                continue;
            if (nsyms == symcap) {
                symcap = symcap ? symcap * 2 : 256;
                syms = realloc(syms, symcap * sizeof *syms);
                if (!syms) abort();
            }
            syms[nsyms++] = sy;
        }
    }

    /* pass 2: each NAME_SRL is a pointer; the variable we edit is what it points to */
    size_t cap = 0, ptrsz = is64 ? 8 : 4;
    for (size_t i = 0; i < nsyms; i++) {
        const sym_t *ps = &syms[i];
        const char *tag;
        size_t base_len = marker_base_len(ps->name, &tag);
        if (!base_len)
            continue;
        bool dup = false;
        for (size_t k = 0; k < i && !dup; k++)
            dup = syms[k].value == ps->value && !strcmp(syms[k].name, ps->name);
        if (dup)
            continue;

        /* the name minus the suffix, as a fallback */
        char base[512];
        snprintf(base, sizeof base, "%.*s", (int)base_len, ps->name);

        const sym_t *tgt = NULL;
        uint64_t target;
        if (ps->size == ptrsz && resolve_pointer(b, secs, shnum, is64, etype, ps, &target)) {
            for (size_t k = 0; k < nsyms; k++) {
                const char *t2;
                if (syms[k].value != target || marker_base_len(syms[k].name, &t2))
                    continue;
                if (!tgt || !strcmp(syms[k].name, base))
                    tgt = &syms[k];
            }
        }
        if (!tgt)
            for (size_t k = 0; k < nsyms && !tgt; k++)
                if (!strcmp(syms[k].name, base))
                    tgt = &syms[k];
        if (!tgt) {
            b->nunresolved++;
            continue;
        }

        bool seen = false;
        for (size_t k = 0; k < b->nvars && !seen; k++)
            seen = b->vars[k].addr == tgt->value && !strcmp(b->vars[k].name, tgt->name);
        if (seen)
            continue;

        const sec_t *sec = &secs[tgt->shndx];
        long long off = -1;
        if (sec->type != SHT_NOBITS) {
            off = file_offset(b, sec, etype, tgt->value, tgt->size);
            if (off < 0)
                continue;
        }

        if (b->nvars == cap) {
            cap = cap ? cap * 2 : 16;
            b->vars = realloc(b->vars, cap * sizeof *b->vars);
            if (!b->vars) abort();
        }
        srl_var *v = &b->vars[b->nvars++];
        memset(v, 0, sizeof *v);
        v->name = xstrdup(tgt->name);
        v->srl_name = xstrdup(ps->name);
        v->tag = *tag ? xstrdup(tag) : NULL;
        v->addr = tgt->value;
        v->size = tgt->size;
        v->section = xstrdup(sec->name);
        v->file_off = off;
        v->orig = xmalloc(v->size);
        if (off >= 0)
            memcpy(v->orig, d + off, v->size);
        else
            memset(v->orig, 0, v->size);
        v->cur = xmalloc(v->size);
        memcpy(v->cur, v->orig, v->size);

        char rn[600];
        snprintf(rn, sizeof rn, "%s" SRL_SUFFIX "_RANGE_MIN", tgt->name);
        v->has_min = read_range_sym(b, secs, etype, find_sym(syms, nsyms, rn), v->rmin_raw, &v->rmin_size);
        snprintf(rn, sizeof rn, "%s" SRL_SUFFIX "_RANGE_MAX", tgt->name);
        v->has_max = read_range_sym(b, secs, etype, find_sym(syms, nsyms, rn), v->rmax_raw, &v->rmax_size);
    }
    free(syms);
    free(secs);
    if (b->nvars)
        qsort(b->vars, b->nvars, sizeof *b->vars, cmp_var);
    return true;

trunc:
    seterr(err, errlen, "truncated or malformed ELF file");
    return false;
}

/* ------------------------------------------------------------------ */
/* type info: from the marker's tag                                   */

static bool is_int_size(size_t n) { return n == 1 || n == 2 || n == 4 || n == 8; }

static void set_kind(srl_var *v, srl_kind k, size_t esz)
{
    v->kind = k;
    v->elem_size = esz;
    v->count = v->size / esz;
}

/* Type from the marker's tag (_SRL_INT, _SRL_FLOAT, _SRL_STR, _SRL_BOOL). The size
 * always comes from the target symbol's st_size. Returns false for an
 * unknown tag, so the caller falls back to debug info. */
static bool apply_tag(srl_var *v)
{
    char tmp[96];
    if (!v->tag)
        return false;
    if (!strcmp(v->tag, "INT")) {
        if (is_int_size(v->size)) {
            set_kind(v, SRL_K_INT, v->size);
            if (v->size == 4) snprintf(tmp, sizeof tmp, "int");
            else snprintf(tmp, sizeof tmp, "int%zu_t", v->size * 8);
        } else {
            set_kind(v, SRL_K_HEX, 1);
            snprintf(tmp, sizeof tmp, "INT? (%zu bytes)", v->size);
        }
    } else if (!strcmp(v->tag, "FLOAT")) {
        if (v->size == 4 || v->size == 8) {
            set_kind(v, SRL_K_FLOAT, v->size);
            snprintf(tmp, sizeof tmp, v->size == 4 ? "float" : "double");
        } else {
            set_kind(v, SRL_K_HEX, 1);
            snprintf(tmp, sizeof tmp, "FLOAT? (%zu bytes)", v->size);
        }
    } else if (!strcmp(v->tag, "BOOL")) {
        if (v->size == 1) {
            set_kind(v, SRL_K_BOOL, 1);
            snprintf(tmp, sizeof tmp, "bool");
        } else {
            set_kind(v, SRL_K_HEX, 1);
            snprintf(tmp, sizeof tmp, "BOOL? (%zu bytes)", v->size);
        }
    } else if (!strcmp(v->tag, "STR")) {
        set_kind(v, SRL_K_STR, 1);
        snprintf(tmp, sizeof tmp, "char[%zu]", v->size);
    } else {
        return false;
    }
    v->type_disp = xstrdup(tmp);
    return true;
}

static int64_t rd_s(const unsigned char *p, size_t n);
static uint64_t rd_u(const unsigned char *p, size_t n);

/* Decode one range bound using the variable's kind. The bound's own size
 * decides float vs double / int width. */
static bool decode_bound(srl_var *v, const unsigned char *raw, size_t sz,
                         int64_t *si, uint64_t *ui, double *fd)
{
    switch (v->kind) {
    case SRL_K_INT:
        if (!is_int_size(sz)) return false;
        *si = rd_s(raw, sz);
        return true;
    case SRL_K_UINT:
        if (!is_int_size(sz)) return false;
        *ui = rd_u(raw, sz);
        return true;
    case SRL_K_FLOAT:
        if (sz == 4) { float f; memcpy(&f, raw, 4); *fd = f; return true; }
        if (sz == 8) { double d; memcpy(&d, raw, 8); *fd = d; return true; }
        return false;
    default:
        return false;
    }
}

static void bound_str(const srl_var *v, bool max, char *buf, size_t n)
{
    bool has = max ? v->has_max : v->has_min;
    if (!has) { snprintf(buf, n, "%s", ""); return; }
    switch (v->kind) {
    case SRL_K_INT:  snprintf(buf, n, "%lld", (long long)(max ? v->max_i : v->min_i)); break;
    case SRL_K_UINT: snprintf(buf, n, "%llu", (unsigned long long)(max ? v->max_u : v->min_u)); break;
    default:         snprintf(buf, n, "%g", max ? v->max_f : v->min_f); break;
    }
}

static void apply_range(srl_var *v)
{
    if (v->has_min)
        v->has_min = decode_bound(v, v->rmin_raw, v->rmin_size, &v->min_i, &v->min_u, &v->min_f);
    if (v->has_max)
        v->has_max = decode_bound(v, v->rmax_raw, v->rmax_size, &v->max_i, &v->max_u, &v->max_f);
    if (!v->has_min && !v->has_max)
        return;
    char lo[64], hi[64], tmp[256];
    bound_str(v, false, lo, sizeof lo);
    bound_str(v, true, hi, sizeof hi);
    snprintf(tmp, sizeof tmp, "%s [%s..%s]", v->type_disp, lo, hi);
    free(v->type_disp);
    v->type_disp = xstrdup(tmp);
}

static void assign_types(srl_binary *b)
{
    size_t untyped = 0;
    for (size_t i = 0; i < b->nvars; i++) {
        srl_var *v = &b->vars[i];
        if (!apply_tag(v)) {
            /* untagged or unknown tag: guess from the size */
            char tmp[64];
            untyped++;
            snprintf(tmp, sizeof tmp, "? (%zu bytes)", v->size);
            v->type_disp = xstrdup(tmp);
            if (is_int_size(v->size))
                set_kind(v, SRL_K_INT, v->size);
            else
                set_kind(v, SRL_K_HEX, 1);
        }
        apply_range(v);
    }
    b->have_types = untyped == 0;
}

/* ------------------------------------------------------------------ */
/* public: load / free                                                 */

srl_binary *srl_load(const char *path, char *err, size_t errlen)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        seterr(err, errlen, "%s: not a regular file", path);
        close(fd);
        return NULL;
    }
    srl_binary *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->len = (size_t)st.st_size;
    b->data = xmalloc(b->len);
    size_t got = 0;
    while (got < b->len) {
        ssize_t r = read(fd, b->data + got, b->len - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    if (got != b->len) {
        seterr(err, errlen, "%s: short read", path);
        srl_free(b);
        return NULL;
    }
    char *rp = realpath(path, NULL);
    b->path = rp ? rp : xstrdup(path);

    if (!parse_elf(b, err, errlen)) {
        srl_free(b);
        return NULL;
    }
    assign_types(b);
    return b;
}

void srl_free(srl_binary *b)
{
    if (!b)
        return;
    for (size_t i = 0; i < b->nvars; i++) {
        free(b->vars[i].name);
        free(b->vars[i].srl_name);
        free(b->vars[i].tag);
        free(b->vars[i].section);
        free(b->vars[i].type_disp);
        free(b->vars[i].orig);
        free(b->vars[i].cur);
    }
    free(b->vars);
    free(b->data);
    free(b->path);
    free(b);
}

bool srl_var_editable(const srl_var *v) { return v->file_off >= 0; }

bool srl_var_modified(const srl_var *v) { return memcmp(v->orig, v->cur, v->size) != 0; }

size_t srl_count_modified(const srl_binary *b)
{
    size_t n = 0;
    for (size_t i = 0; i < b->nvars; i++)
        n += srl_var_modified(&b->vars[i]);
    return n;
}

void srl_revert(srl_binary *b)
{
    for (size_t i = 0; i < b->nvars; i++)
        memcpy(b->vars[i].cur, b->vars[i].orig, b->vars[i].size);
}

/* ------------------------------------------------------------------ */
/* formatting                                                          */

static int64_t rd_s(const unsigned char *p, size_t n)
{
    switch (n) {
    case 1: { int8_t x; memcpy(&x, p, 1); return x; }
    case 2: { int16_t x; memcpy(&x, p, 2); return x; }
    case 4: { int32_t x; memcpy(&x, p, 4); return x; }
    default: { int64_t x; memcpy(&x, p, 8); return x; }
    }
}

static uint64_t rd_u(const unsigned char *p, size_t n)
{
    switch (n) {
    case 1: { uint8_t x; memcpy(&x, p, 1); return x; }
    case 2: { uint16_t x; memcpy(&x, p, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, p, 4); return x; }
    default: { uint64_t x; memcpy(&x, p, 8); return x; }
    }
}

static void wr_u(unsigned char *p, size_t n, uint64_t v)
{
    switch (n) {
    case 1: { uint8_t x = (uint8_t)v; memcpy(p, &x, 1); break; }
    case 2: { uint16_t x = (uint16_t)v; memcpy(p, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy(p, &x, 4); break; }
    default: memcpy(p, &v, 8);
    }
}

/* shortest representation that round-trips */
static void fmt_float(FILE *f, double d, bool single)
{
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        double back = strtod(buf, NULL);
        if (single ? (float)back == (float)d : back == d)
            break;
    }
    /* make it obviously a float */
    if (isfinite(d) && !strpbrk(buf, ".eEn"))
        strncat(buf, ".0", sizeof buf - strlen(buf) - 1);
    fputs(buf, f);
}

char *srl_format(const srl_var *v) { return srl_format_bytes(v, v->cur); }

char *srl_format_bytes(const srl_var *v, const unsigned char *bytes)
{
    char *s = NULL;
    size_t sl = 0;
    FILE *f = open_memstream(&s, &sl);
    if (!f) abort();

    if (v->kind == SRL_K_STR) {
        for (size_t i = 0; i < v->size && bytes[i]; i++) {
            unsigned char c = bytes[i];
            if (c == '\\') fputs("\\\\", f);
            else if (c == '\n') fputs("\\n", f);
            else if (c == '\t') fputs("\\t", f);
            else if (c >= 0x20 && c < 0x7f) fputc(c, f);
            else fprintf(f, "\\x%02x", c);
        }
    } else if (v->kind == SRL_K_HEX) {
        for (size_t i = 0; i < v->size; i++)
            fprintf(f, "%s%02x", i ? " " : "", bytes[i]);
    } else {
        for (size_t i = 0; i < v->count; i++) {
            const unsigned char *p = bytes + i * v->elem_size;
            if (i)
                fputs(", ", f);
            switch (v->kind) {
            case SRL_K_INT:  fprintf(f, "%lld", (long long)rd_s(p, v->elem_size)); break;
            case SRL_K_UINT: fprintf(f, "%llu", (unsigned long long)rd_u(p, v->elem_size)); break;
            case SRL_K_PTR:  fprintf(f, "0x%llx", (unsigned long long)rd_u(p, v->elem_size)); break;
            case SRL_K_BOOL: {
                unsigned x = p[0];
                if (x <= 1) fputs(x ? "true" : "false", f);
                else fprintf(f, "%u", x);
                break;
            }
            case SRL_K_FLOAT:
                if (v->elem_size == 4) { float x; memcpy(&x, p, 4); fmt_float(f, x, true); }
                else { double x; memcpy(&x, p, 8); fmt_float(f, x, false); }
                break;
            default: break;
            }
        }
    }
    fclose(f);
    return s;
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */

/* decimal by default (so "010" is ten), 0x / 0b / 0o prefixes allowed */
static bool parse_int_tok(const char *tok, bool want_unsigned, int64_t *sv, uint64_t *uv)
{
    const char *p = tok;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
    }
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) { base = 8; p += 2; }
    if (!*p || !isalnum((unsigned char)*p))
        return false;
    errno = 0;
    char *end;
    unsigned long long mag = strtoull(p, &end, base);
    if (*end || errno == ERANGE)
        return false;
    if (want_unsigned) {
        if (neg && mag)
            return false;
        *uv = mag;
    } else {
        if (neg) {
            if (mag > (unsigned long long)INT64_MAX + 1)
                return false;
            *sv = mag == (unsigned long long)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
        } else {
            if (mag > INT64_MAX)
                return false;
            *sv = (int64_t)mag;
        }
    }
    return true;
}

static bool range_err(const srl_var *v, const char *tok, char *err, size_t errlen)
{
    char lo[64], hi[64];
    bound_str(v, false, lo, sizeof lo);
    bound_str(v, true, hi, sizeof hi);
    if (v->has_min && v->has_max)
        seterr(err, errlen, "%s is outside %s's range [%s..%s]", tok, v->name, lo, hi);
    else if (v->has_min)
        seterr(err, errlen, "%s is below %s's minimum %s", tok, v->name, lo);
    else
        seterr(err, errlen, "%s is above %s's maximum %s", tok, v->name, hi);
    return false;
}

static bool parse_elem(const srl_var *v, const char *tok, unsigned char *out, char *err, size_t errlen)
{
    size_t n = v->elem_size;
    switch (v->kind) {
    case SRL_K_INT: {
        int64_t x;
        if (!parse_int_tok(tok, false, &x, NULL)) {
            seterr(err, errlen, "'%s' is not an integer", tok);
            return false;
        }
        int64_t lo = n == 8 ? INT64_MIN : -((int64_t)1 << (n * 8 - 1));
        int64_t hi = n == 8 ? INT64_MAX : ((int64_t)1 << (n * 8 - 1)) - 1;
        if (x < lo || x > hi) {
            seterr(err, errlen, "%s is out of range [%lld, %lld]", tok, (long long)lo, (long long)hi);
            return false;
        }
        if ((v->has_min && x < v->min_i) || (v->has_max && x > v->max_i))
            return range_err(v, tok, err, errlen);
        wr_u(out, n, (uint64_t)x);
        return true;
    }
    case SRL_K_UINT:
    case SRL_K_PTR: {
        uint64_t x;
        if (!parse_int_tok(tok, true, NULL, &x)) {
            seterr(err, errlen, "'%s' is not an unsigned integer", tok);
            return false;
        }
        uint64_t hi = n == 8 ? UINT64_MAX : ((uint64_t)1 << (n * 8)) - 1;
        if (x > hi) {
            seterr(err, errlen, "%s is out of range [0, %llu]", tok, (unsigned long long)hi);
            return false;
        }
        if (v->kind == SRL_K_UINT && ((v->has_min && x < v->min_u) || (v->has_max && x > v->max_u)))
            return range_err(v, tok, err, errlen);
        wr_u(out, n, x);
        return true;
    }
    case SRL_K_BOOL:
        if (!strcasecmp(tok, "true") || !strcmp(tok, "1")) out[0] = 1;
        else if (!strcasecmp(tok, "false") || !strcmp(tok, "0")) out[0] = 0;
        else {
            seterr(err, errlen, "'%s' is not a bool (true/false/1/0)", tok);
            return false;
        }
        return true;
    case SRL_K_FLOAT: {
        errno = 0;
        char *end;
        double d = strtod(tok, &end);
        if (!*tok || *end) {
            seterr(err, errlen, "'%s' is not a number", tok);
            return false;
        }
        if (n == 4)
            d = (float)d; /* check the value that will actually be stored */
        if (isnan(d) && (v->has_min || v->has_max)) {
            seterr(err, errlen, "nan is not allowed for a ranged variable");
            return false;
        }
        if ((v->has_min && d < v->min_f) || (v->has_max && d > v->max_f))
            return range_err(v, tok, err, errlen);
        if (n == 4) {
            float f = (float)d;
            memcpy(out, &f, 4);
        } else {
            memcpy(out, &d, 8);
        }
        return true;
    }
    default:
        seterr(err, errlen, "internal: bad kind");
        return false;
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool srl_parse(const srl_var *v, const char *text, unsigned char *out, char *err, size_t errlen)
{
    memset(out, 0, v->size);

    if (v->kind == SRL_K_STR) {
        size_t o = 0;
        for (const char *p = text; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case '0': c = 0; break;
                case '\\': c = '\\'; break;
                case 'x': {
                    int h = hexval(p[1]), l = h >= 0 ? hexval(p[2]) : -1;
                    if (h < 0 || l < 0) {
                        seterr(err, errlen, "bad \\x escape (use \\xNN)");
                        return false;
                    }
                    c = (unsigned char)(h * 16 + l);
                    p += 2;
                    break;
                }
                default: c = (unsigned char)*p;
                }
            }
            if (o + 1 >= v->size) { /* keep the terminating NUL */
                seterr(err, errlen, "too long: %s holds at most %zu characters", v->name, v->size - 1);
                return false;
            }
            out[o++] = c;
        }
        return true;
    }

    if (v->kind == SRL_K_HEX) {
        size_t o = 0;
        int hi = -1;
        for (const char *p = text; *p; p++) {
            if (isspace((unsigned char)*p) || *p == ',')
                continue;
            int h = hexval(*p);
            if (h < 0) {
                seterr(err, errlen, "'%c' is not a hex digit", *p);
                return false;
            }
            if (hi < 0) {
                hi = h;
            } else {
                if (o >= v->size) {
                    seterr(err, errlen, "too many bytes (expected %zu)", v->size);
                    return false;
                }
                out[o++] = (unsigned char)(hi * 16 + h);
                hi = -1;
            }
        }
        if (hi >= 0 || o != v->size) {
            seterr(err, errlen, "expected exactly %zu hex bytes", v->size);
            return false;
        }
        return true;
    }

    /* scalar or array of scalars: comma separated, optional { } */
    char *copy = xstrdup(text), *s = trim(copy);
    size_t L = strlen(s);
    if (L >= 2 && s[0] == '{' && s[L - 1] == '}') {
        s[L - 1] = 0;
        s = trim(s + 1);
    }
    size_t got = 0;
    bool ok = true;
    char *save = NULL;
    for (char *tok = strtok_r(s, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok = trim(tok);
        if (got >= v->count) {
            got++;
            continue;
        }
        if (!parse_elem(v, tok, out + got * v->elem_size, err, errlen)) {
            ok = false;
            break;
        }
        got++;
    }
    if (ok && got != v->count) {
        seterr(err, errlen, "expected %zu value%s, got %zu", v->count, v->count == 1 ? "" : "s", got);
        ok = false;
    }
    free(copy);
    return ok;
}

/* ------------------------------------------------------------------ */
/* patch into memfd, fork + exec                                       */

/* Write the original bytes plus every patch into fd. */
static bool write_image(const srl_binary *b, int fd, char *err, size_t errlen)
{
    size_t done = 0;
    while (done < b->len) {
        ssize_t w = write(fd, b->data + done, b->len - done);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            seterr(err, errlen, "write: %s", strerror(errno));
            return false;
        }
        done += (size_t)w;
    }
    for (size_t i = 0; i < b->nvars; i++) {
        const srl_var *v = &b->vars[i];
        if (!srl_var_editable(v) || !srl_var_modified(v))
            continue;
        if (pwrite(fd, v->cur, v->size, v->file_off) != (ssize_t)v->size) {
            seterr(err, errlen, "patch %s: %s", v->name, strerror(errno));
            return false;
        }
    }
    return true;
}

int srl_make_memfd(const srl_binary *b, char *err, size_t errlen)
{
    const char *base = strrchr(b->path, '/');
    base = base ? base + 1 : b->path;
    int fd = memfd_create(base, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        seterr(err, errlen, "memfd_create: %s", strerror(errno));
        return -1;
    }
    if (!write_image(b, fd, err, errlen)) {
        close(fd);
        return -1;
    }
    /* freeze it: the image can't be changed after this point */
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    return fd;
}

bool srl_save(const srl_binary *b, const char *path, char *err, size_t errlen)
{
    struct stat orig, dst;
    mode_t mode = 0755;
    if (stat(b->path, &orig) == 0) {
        mode = orig.st_mode & 07777;
        if (stat(path, &dst) == 0 && dst.st_dev == orig.st_dev && dst.st_ino == orig.st_ino) {
            seterr(err, errlen, "refusing to overwrite the original binary");
            return false;
        }
    }
    /* write a temp file next to the target, then rename: never a half-written binary */
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        return false;
    }
    bool ok = write_image(b, fd, err, errlen);
    if (ok && fchmod(fd, mode) < 0) {
        seterr(err, errlen, "chmod: %s", strerror(errno));
        ok = false;
    }
    if (close(fd) < 0 && ok) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        ok = false;
    }
    if (ok && rename(tmp, path) < 0) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        ok = false;
    }
    if (!ok)
        unlink(tmp);
    return ok;
}

pid_t srl_spawn(int exe_fd, int tty_fd, char *const argv[], char *const envp[], const char *cwd)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigset_t none;
    sigemptyset(&none);
    static const char msg[] = "\r\nsrl_editor: exec of patched image failed\r\n";

    pid_t pid = fork();
    if (pid != 0)
        return pid; /* parent, or -1 */

    /* child: async-signal-safe calls only from here on */
    for (int s = 1; s < NSIG; s++)
        sigaction(s, &dfl, NULL);
    sigprocmask(SIG_SETMASK, &none, NULL);
    setsid();
    if (tty_fd >= 0) {
        ioctl(tty_fd, TIOCSCTTY, 0);
        dup2(tty_fd, 0);
        dup2(tty_fd, 1);
        dup2(tty_fd, 2);
        if (tty_fd > 2)
            close(tty_fd);
    }
    if (cwd && chdir(cwd) < 0) {
        /* keep going in the inherited cwd */
    }
    fexecve(exe_fd, argv, envp);
    ssize_t ignored = write(2, msg, sizeof msg - 1);
    (void)ignored;
    _exit(127);
}

/* ------------------------------------------------------------------ */
/* runtime editing                                                     */

void srl_live_init(srl_live *L, pid_t pid, int exe_fd)
{
    struct stat st;
    memset(L, 0, sizeof *L);
    L->pid = pid;
    if (exe_fd >= 0 && fstat(exe_fd, &st) == 0)
        L->ino = (unsigned long)st.st_ino;
}

bool srl_live_locate(srl_live *L, const srl_binary *b, char *err, size_t errlen)
{
    if (L->ready)
        return true;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)L->pid);
    FILE *f = fopen(path, "re");
    if (!f) {
        seterr(err, errlen, "%s: %s", path, strerror(errno));
        return false;
    }
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        unsigned long long start, end, off;
        unsigned long ino;
        unsigned maj, mnr;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s %llx %x:%x %lu", &start, &end, perms, &off, &maj, &mnr, &ino) != 7)
            continue;
        /* the memfd we exec'd, at file offset 0 (the first PT_LOAD) */
        if (ino != L->ino || off != 0)
            continue;
        L->bias = b->pie ? start - b->load_vaddr : 0;
        L->ready = true;
        break;
    }
    fclose(f);
    if (!L->ready)
        seterr(err, errlen, "the program hasn't been loaded yet");
    return L->ready;
}

static bool live_io(srl_live *L, const srl_binary *b, const srl_var *v, void *buf, bool write,
                    char *err, size_t errlen)
{
    if (!srl_live_locate(L, b, err, errlen))
        return false;
    struct iovec local = { buf, v->size };
    struct iovec remote = { (void *)(uintptr_t)(L->bias + v->addr), v->size };
    ssize_t r = write ? process_vm_writev(L->pid, &local, 1, &remote, 1, 0)
                      : process_vm_readv(L->pid, &local, 1, &remote, 1, 0);
    if (r == (ssize_t)v->size)
        return true;
    if (r < 0 && errno == EPERM)
        seterr(err, errlen, "no permission to access pid %d (check /proc/sys/kernel/yama/ptrace_scope)", (int)L->pid);
    else if (r < 0)
        seterr(err, errlen, "%s %s: %s", write ? "write" : "read", v->name, strerror(errno));
    else
        seterr(err, errlen, "%s %s: short transfer", write ? "write" : "read", v->name);
    return false;
}

bool srl_live_read(srl_live *L, const srl_binary *b, const srl_var *v,
                   unsigned char *out, char *err, size_t errlen)
{
    return live_io(L, b, v, out, false, err, errlen);
}

bool srl_live_write(srl_live *L, const srl_binary *b, const srl_var *v,
                    const unsigned char *bytes, char *err, size_t errlen)
{
    return live_io(L, b, v, (void *)bytes, true, err, errlen);
}
