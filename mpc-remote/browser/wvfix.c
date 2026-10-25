/* wvfix: make Widevine (libwidevinecdm.so, 32-bit ARM, built for ChromeOS) load with the MPC's glibc.
 *   wvfix <libwidevinecdm.so>      patches the file in place; exit status 0 = done (or nothing to do)
 * The library packs its relocations in the DT_RELR format but doesn't list the GLIBC_ABI_DT_RELR version that glibc
 * 2.36 and later require then ("DT_RELR without GLIBC_ABI_DT_RELR dependency"). This adds that version to what it
 * needs from libc.so.6: its version list (.gnu.version_r) and string table (.dynstr) are copied, with the new entry,
 * to the end of the file, into a new loaded segment (in place of the PT_NOTE header, which loading doesn't use), and
 * the dynamic section points to the copies. Nothing else in the library changes. */
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN 0x10000u
#ifndef DT_RELR
#define DT_RELR 36
#endif

static uint8_t *f;
static size_t fn;
static Elf32_Phdr *ph;
static int phn;

static int fail(const char *why) { fprintf(stderr, "wvfix: %s\n", why); return 1; }

static long off_of(uint32_t vaddr) {   /* file offset of a loaded address */
    for (int i = 0; i < phn; i++)
        if (ph[i].p_type == PT_LOAD && vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz)
            return (long)(vaddr - ph[i].p_vaddr + ph[i].p_offset);
    return -1;
}

static uint32_t elf_hash(const char *s) {
    uint32_t h = 0, g;
    while (*s) { h = (h << 4) + (uint8_t)*s++; if ((g = h & 0xf0000000u)) h ^= g >> 24; h &= ~g; }
    return h;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: wvfix <libwidevinecdm.so>\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    fn = (size_t)ftell(in);
    rewind(in);
    f = malloc(fn);
    if (!f || fread(f, 1, fn, in) != fn) return fail("can't read it");
    fclose(in);
    Elf32_Ehdr *eh = (Elf32_Ehdr *)f;
    if (fn < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_machine != EM_ARM || eh->e_phentsize != sizeof(Elf32_Phdr) || eh->e_phoff + eh->e_phnum * sizeof *ph > fn)
        return fail("not a 32-bit ARM library");
    ph = (Elf32_Phdr *)(f + eh->e_phoff);
    phn = eh->e_phnum;

    /* the dynamic section: has DT_RELR? where are the strings and the version list? */
    Elf32_Dyn *dyn = NULL;
    int dn = 0;
    for (int i = 0; i < phn; i++)
        if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset + ph[i].p_filesz <= fn) {
            dyn = (Elf32_Dyn *)(f + ph[i].p_offset);
            dn = (int)(ph[i].p_filesz / sizeof *dyn);
        }
    if (!dyn) return fail("no dynamic section");
    Elf32_Dyn *strtab = NULL, *strsz = NULL, *verneed = NULL, *verneednum = NULL;
    int relr = 0;
    for (int i = 0; i < dn && dyn[i].d_tag != DT_NULL; i++) {
        if (dyn[i].d_tag == DT_STRTAB) strtab = &dyn[i];
        else if (dyn[i].d_tag == DT_STRSZ) strsz = &dyn[i];
        else if (dyn[i].d_tag == DT_VERNEED) verneed = &dyn[i];
        else if (dyn[i].d_tag == DT_VERNEEDNUM) verneednum = &dyn[i];
        else if (dyn[i].d_tag == DT_RELR) relr = 1;
    }
    if (!relr) { printf("wvfix: no DT_RELR: nothing to do\n"); return 0; }
    if (!strtab || !strsz || !verneed || !verneednum) return fail("no version list");
    const long so = off_of(strtab->d_un.d_ptr), vo = off_of(verneed->d_un.d_ptr);
    if (so < 0 || vo < 0 || (size_t)so + strsz->d_un.d_val > fn) return fail("odd layout");
    const char *str = (const char *)(f + so);

    /* walk the version list: its size, libc.so.6's entry and its last version, the highest version index */
    size_t vlen = 0;
    long libc_last = -1;
    unsigned maxndx = 1;
    Elf32_Verneed *libc = NULL;
    long p = 0;
    for (unsigned n = 0; n < verneednum->d_un.d_val; n++) {
        if (vo + p + (long)sizeof(Elf32_Verneed) > (long)fn) return fail("odd version list");
        Elf32_Verneed *vn = (Elf32_Verneed *)(f + vo + p);
        if (vn->vn_file >= strsz->d_un.d_val) return fail("odd version list");
        long a = p + vn->vn_aux;
        for (unsigned k = 0; k < vn->vn_cnt; k++) {
            if (vo + a + (long)sizeof(Elf32_Vernaux) > (long)fn) return fail("odd version list");
            Elf32_Vernaux *va = (Elf32_Vernaux *)(f + vo + a);
            if (va->vna_name < strsz->d_un.d_val && !strcmp(str + va->vna_name, "GLIBC_ABI_DT_RELR")) {
                printf("wvfix: already done\n");
                return 0;
            }
            if ((va->vna_other & 0x7fff) > maxndx) maxndx = va->vna_other & 0x7fff;
            if ((size_t)(a + sizeof *va) > vlen) vlen = (size_t)(a + sizeof *va);
            if (!strcmp(str + vn->vn_file, "libc.so.6") && k == vn->vn_cnt - 1u) { libc = vn; libc_last = a; }
            a += va->vna_next;
        }
        if ((size_t)(p + sizeof *vn) > vlen) vlen = (size_t)(p + sizeof *vn);
        if (!vn->vn_next) break;
        p += vn->vn_next;
    }
    if (!libc) return fail("it doesn't use libc.so.6");

    /* a header slot for the new segment */
    Elf32_Phdr *slot = NULL;
    uint32_t vend = 0;
    for (int i = 0; i < phn; i++) {
        if (ph[i].p_type == PT_NOTE && !slot) slot = &ph[i];
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > vend) vend = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (!slot) return fail("no spare program header");

    /* the new segment: strings + "GLIBC_ABI_DT_RELR", then the version list + one entry */
    const size_t base = (fn + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    const uint32_t vbase = (vend + ALIGN - 1) & ~(ALIGN - 1);
    const size_t s_len = strsz->d_un.d_val, name_off = s_len, s_new = s_len + sizeof "GLIBC_ABI_DT_RELR";
    const size_t v_at = (s_new + 3) & ~(size_t)3, total = v_at + vlen + sizeof(Elf32_Vernaux);
    uint8_t *out = calloc(1, base + total);
    if (!out) return fail("out of memory");
    memcpy(out, f, fn);
    uint8_t *seg = out + base;
    memcpy(seg, str, s_len);
    memcpy(seg + name_off, "GLIBC_ABI_DT_RELR", sizeof "GLIBC_ABI_DT_RELR");
    memcpy(seg + v_at, f + vo, vlen);
    Elf32_Vernaux *last = (Elf32_Vernaux *)(seg + v_at + libc_last);
    Elf32_Vernaux *add = (Elf32_Vernaux *)(seg + v_at + vlen);
    Elf32_Verneed *nlibc = (Elf32_Verneed *)(seg + v_at + ((uint8_t *)libc - (f + vo)));
    last->vna_next = (Elf32_Word)(vlen - (size_t)libc_last);
    add->vna_hash = elf_hash("GLIBC_ABI_DT_RELR");
    add->vna_flags = 0;
    add->vna_other = (Elf32_Half)(maxndx + 1);
    add->vna_name = (Elf32_Word)name_off;
    add->vna_next = 0;
    nlibc->vn_cnt++;

    /* point the library at them (headers and dynamic section, in the copy) */
    Elf32_Phdr *nslot = (Elf32_Phdr *)(out + ((uint8_t *)slot - f));
    nslot->p_type = PT_LOAD;
    nslot->p_flags = PF_R;
    nslot->p_offset = (Elf32_Off)base;
    nslot->p_vaddr = nslot->p_paddr = vbase;
    nslot->p_filesz = nslot->p_memsz = (Elf32_Word)total;
    nslot->p_align = ALIGN;
    const size_t d = (size_t)((uint8_t *)dyn - f);
    Elf32_Dyn *ndyn = (Elf32_Dyn *)(out + d);
    ndyn[strtab - dyn].d_un.d_ptr = vbase;
    ndyn[strsz - dyn].d_un.d_val = (Elf32_Word)s_new;
    ndyn[verneed - dyn].d_un.d_ptr = vbase + (Elf32_Addr)v_at;
    /* (and the section table, so tools still read it right) */
    Elf32_Ehdr *neh = (Elf32_Ehdr *)out;
    if (neh->e_shoff && neh->e_shentsize == sizeof(Elf32_Shdr) && neh->e_shoff + neh->e_shnum * sizeof(Elf32_Shdr) <= fn) {
        Elf32_Shdr *sh = (Elf32_Shdr *)(out + neh->e_shoff);
        for (int i = 0; i < neh->e_shnum; i++) {
            if (sh[i].sh_type == SHT_STRTAB && sh[i].sh_addr == strtab->d_un.d_ptr) {
                sh[i].sh_addr = vbase; sh[i].sh_offset = (Elf32_Off)base; sh[i].sh_size = (Elf32_Word)s_new;
            } else if (sh[i].sh_type == SHT_GNU_verneed) {
                sh[i].sh_addr = vbase + (Elf32_Addr)v_at; sh[i].sh_offset = (Elf32_Off)(base + v_at);
                sh[i].sh_size = (Elf32_Word)(vlen + sizeof(Elf32_Vernaux));
            }
        }
    }

    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.wvfix", argv[1]);
    FILE *o = fopen(tmp, "wb");
    if (!o || fwrite(out, 1, base + total, o) != base + total || fclose(o)) { remove(tmp); return fail("can't write it"); }
    if (rename(tmp, argv[1])) { remove(tmp); return fail("can't replace it"); }
    printf("wvfix: added GLIBC_ABI_DT_RELR (version %u) for libc.so.6\n", maxndx + 1);
    return 0;
}
