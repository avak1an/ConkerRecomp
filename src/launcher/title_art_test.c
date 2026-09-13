/* Native reader test driver; synthetic inputs are created by Python tests. */
#include "title_art.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3 && argc != 6) return 2;
    HBITMAP image = conker_disc_title_art(argv[1], RGB(17, 19, 25));
    int expected = _wtoi(argv[2]);
    if (!image) { puts("no artwork"); return expected ? 1 : 0; }
    DIBSECTION info = {0}; GetObject(image, sizeof(info), &info);
    printf("artwork %ld x %ld\n", info.dsBm.bmWidth, info.dsBm.bmHeight);
    int ok = expected;
    if (argc == 6) {
        ok = ok && info.dsBm.bmWidth == _wtoi(argv[3]) && info.dsBm.bmHeight == _wtoi(argv[4]);
        ok = ok && info.dsBm.bmBits && *(uint32_t *)info.dsBm.bmBits == wcstoul(argv[5], NULL, 16);
    }
    DeleteObject(image); return ok ? 0 : 1;
}
