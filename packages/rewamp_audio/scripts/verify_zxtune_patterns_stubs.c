/* Ce que l'app fournit et dont le harnais zxtune n'a pas besoin. Fichier à
 * part de verify_pt3_patterns_stubs.c: celui-là redéfinit aussi
 * rewamp_waveform_pos/_read_i8, que rewamp_channel_data.c (lié ici) porte
 * déjà — doublon de symbole au lien. Compilé en C (linkage C). */
#include <strings.h>
int rewamp_ext_in_list(const char* ext, const char* const* list) {
    if (!ext) return 0;
    for (; *list; ++list) if (!strcasecmp(ext, *list)) return 1;
    return 0;
}
/* Boucle forcée: lue par le greffon à l'ouverture, posée par l'app. */
int g_force_loop_mode = 0;
int g_force_loop_native_veto = 0;
