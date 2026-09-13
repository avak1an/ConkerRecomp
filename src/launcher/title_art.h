#ifndef CONKER_TITLE_ART_H
#define CONKER_TITLE_ART_H
#include <windows.h>
/* Reads only the title resource from the user's ISO; never writes a file.
 * The returned bitmap belongs to the caller. Preview is not version approval. */
HBITMAP conker_disc_title_art(const wchar_t *iso_path, COLORREF background);
#endif
