/* Bounded, read-only XDVDFS -> default.xbe -> $$XTIMAGE -> XPR0/BC1 reader.
 * Format-only code: no embedded image bytes or generated game source.
 * The Conker title uses a 128x128 BC1 resource. Other resource formats are
 * rejected instead of attempting to interpret them as pixels.
 */
#define WIN32_LEAN_AND_MEAN
#include "title_art.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048u
#define MAX_ROOT (4u * 1024u * 1024u)
#define MAX_HEADERS (1024u * 1024u)
#define MAX_XPR (1024u * 1024u)

static uint32_t u32(const unsigned char *p)
{ return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static unsigned u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static int span(uint64_t size, uint64_t offset, uint64_t length)
{ return offset <= size && length <= size - offset; }
static int read_at(HANDLE file, uint64_t size, uint64_t offset, void *out, DWORD length)
{
    LARGE_INTEGER where; DWORD got = 0;
    if (!span(size, offset, length)) return 0;
    where.QuadPart = (LONGLONG)offset;
    return SetFilePointerEx(file, where, NULL, FILE_BEGIN) &&
        ReadFile(file, out, length, &got, NULL) && got == length;
}

static int find_xbe(HANDLE file, uint64_t file_size, uint64_t *xbe_offset, uint32_t *xbe_size)
{
    static const uint64_t bases[] = {0, 0xFD90, 0x30600, 0xFD90000, 0x18300000};
    unsigned char descriptor[SECTOR]; unsigned char *root = NULL;
    uint64_t base = 0; int found = 0;
    for (unsigned i = 0; i < sizeof(bases)/sizeof(bases[0]); ++i) {
        if (read_at(file, file_size, bases[i] + 32u * SECTOR, descriptor, sizeof(descriptor)) &&
            memcmp(descriptor, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
            base = bases[i]; found = 1; break;
        }
    }
    if (!found) return 0;
    uint32_t root_size = u32(descriptor + 0x18);
    uint64_t root_offset = base + (uint64_t)u32(descriptor + 0x14) * SECTOR;
    if (!root_size || root_size > MAX_ROOT || !span(file_size, root_offset, root_size)) return 0;
    root = malloc(root_size);
    if (!root) return 0;
    found = 0;
    if (!read_at(file, file_size, root_offset, root, root_size)) goto done;
    /* Entries are packed into sectors; stop at each sector's padding. No
     * recursion, tree-link following or extraction of untrusted filenames. */
    for (uint32_t sector = 0; sector < root_size; sector += SECTOR) {
        uint32_t end = root_size - sector < SECTOR ? root_size : sector + SECTOR;
        for (uint32_t pos = sector; span(end, pos, 14);) {
            const unsigned char *entry = root + pos;
            unsigned length = entry[13];
            if (!length || (u16(entry) == 0xFFFF && u16(entry + 2) == 0xFFFF)) break;
            if (!span(end, pos + 14u, length)) { found = 0; goto done; }
            if (length == 11 && _strnicmp((const char *)entry + 14, "default.xbe", 11) == 0) {
                if (found || (entry[12] & 0x10)) { found = 0; goto done; }
                uint64_t offset = base + (uint64_t)u32(entry + 4) * SECTOR;
                uint32_t size = u32(entry + 8);
                if (!span(file_size, offset, size) || size < 0x178) goto done;
                *xbe_offset = offset; *xbe_size = size; found = 1;
            }
            pos += (14u + length + 3u) & ~3u;
        }
    }
done:
    free(root); return found;
}

static void rgb565(unsigned value, unsigned char out[4])
{
    out[0] = (unsigned char)((value & 31) * 255 / 31);
    out[1] = (unsigned char)(((value >> 5) & 63) * 255 / 63);
    out[2] = (unsigned char)((value >> 11) * 255 / 31); out[3] = 255;
}
static HBITMAP decode_title_xpr(const unsigned char *xpr, uint32_t size, COLORREF bg)
{
    if (size < 32 || memcmp(xpr, "XPR0", 4)) return NULL;
    uint32_t total = u32(xpr + 4), headers = u32(xpr + 8), format = u32(xpr + 24);
    if (total > size || headers < 32 || headers > total || (u32(xpr + 12) & 0x70000) != 0x40000 ||
        ((format >> 8) & 0xFF) != 0x0C || ((format >> 4) & 0xF) != 2) return NULL;
    unsigned width = 1u << ((format >> 20) & 15), height = 1u << ((format >> 24) & 15);
    if (width < 4 || height < 4 || width > 1024 || height > 1024) return NULL;
    uint64_t data = (uint64_t)headers + u32(xpr + 16);
    if (!span(total, data, (uint64_t)width * height / 2)) return NULL;
    BITMAPINFO bi = {0}; void *pixels;
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width; bi.bmiHeader.biHeight = -(LONG)height;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HBITMAP bitmap = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!bitmap) return NULL;
    for (unsigned by = 0; by < height; by += 4) for (unsigned bx = 0; bx < width; bx += 4) {
        const unsigned char *block = xpr + (size_t)data + ((by / 4) * (width / 4) + bx / 4) * 8;
        unsigned a = u16(block), b = u16(block + 2); uint32_t codes = u32(block + 4);
        unsigned char colors[4][4]; rgb565(a, colors[0]); rgb565(b, colors[1]);
        for (unsigned k = 0; k < 3; ++k) {
            colors[2][k] = (unsigned char)(a > b ? (2 * colors[0][k] + colors[1][k]) / 3 :
                                                               (colors[0][k] + colors[1][k]) / 2);
            colors[3][k] = (unsigned char)(a > b ? (colors[0][k] + 2 * colors[1][k]) / 3 : 0);
        }
        colors[2][3] = 255; colors[3][3] = a > b ? 255 : 0;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned char *pixel = (unsigned char *)pixels + ((by + i / 4) * width + bx + i % 4) * 4;
            const unsigned char *color = colors[(codes >> (i * 2)) & 3];
            pixel[0] = color[3] ? color[0] : GetBValue(bg);
            pixel[1] = color[3] ? color[1] : GetGValue(bg);
            pixel[2] = color[3] ? color[2] : GetRValue(bg); pixel[3] = 255;
        }
    }
    return bitmap;
}

HBITMAP conker_disc_title_art(const wchar_t *path, COLORREF background)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER file_size; uint64_t xbe_offset; uint32_t xbe_size;
    unsigned char first[0x178], *header = NULL, *xpr = NULL; HBITMAP bitmap = NULL;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 ||
        !find_xbe(file, file_size.QuadPart, &xbe_offset, &xbe_size) ||
        !read_at(file, file_size.QuadPart, xbe_offset, first, sizeof(first)) || memcmp(first, "XBEH", 4)) goto done;
    uint32_t base = u32(first + 0x104), size = u32(first + 0x108), count = u32(first + 0x11C);
    uint32_t table_va = u32(first + 0x120), cert_va = u32(first + 0x118);
    if (size < sizeof(first) || size > MAX_HEADERS || size > xbe_size || !count || count > 96 ||
        table_va < base || cert_va < base || !span(size, table_va - base, (uint64_t)count * 0x38) ||
        !span(size, cert_va - base, 12)) goto done;
    header = malloc(size);
    if (!header || !read_at(file, file_size.QuadPart, xbe_offset, header, size)) goto done;
    /* Title identity selects this game's preview; SHA-1 verification is still
     * a separate step and is never satisfied by finding an image resource. */
    if (u32(header + cert_va - base + 8) != 0x4D530051) goto done;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *section = header + table_va - base + i * 0x38;
        uint32_t name_va = u32(section + 0x14);
        if (name_va < base || !span(size, name_va - base, 10)) continue;
        if (memcmp(header + name_va - base, "$$XTIMAGE\0", 10)) continue;
        uint32_t offset = u32(section + 0xC), length = u32(section + 0x10);
        if (!length || length > MAX_XPR || !span(xbe_size, offset, length)) goto done;
        xpr = malloc(length);
        if (xpr && read_at(file, file_size.QuadPart, xbe_offset + offset, xpr, length))
            bitmap = decode_title_xpr(xpr, length, background);
        break;
    }
done:
    free(xpr); free(header); CloseHandle(file); return bitmap;
}
