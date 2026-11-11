/* Pure data IME packs. Worker owns prepared handles until main-thread activation. */
#ifndef MELODY_IME_PACK_H
#define MELODY_IME_PACK_H
#include <stdint.h>
#define IME_PACK_FORMAT 1u
#define IME_PACK_RECORD_SIZE 80u
#define IME_PACK_MAX_RECORDS 65535u
#define IME_PACK_HEADER_SIZE 64u
#define IME_PACK_MAX_BYTES (IME_PACK_HEADER_SIZE + IME_PACK_MAX_RECORDS * IME_PACK_RECORD_SIZE)
enum { IME_PACK_OK=0, IME_PACK_IO=-1, IME_PACK_FORMAT_ERROR=-2,
       IME_PACK_CRC=-3, IME_PACK_BOUNDS=-4, IME_PACK_FONT=-5, IME_PACK_MEMORY=-6 };
enum { IME_PACK_PINYIN=0, IME_PACK_KANA=1, IME_PACK_CHAR=2 };
typedef struct ImePack ImePack;
typedef struct { char key[48]; char word[24]; uint32_t freq; uint32_t kind; } ImePackEntry;
/* Immutable snapshot validation: exact size, version, CRC, sorted records, UTF-8 and builtin font. */
int ime_pack_prepare(const char *path, ImePack **out);
int ime_pack_validate(const char *path);
void ime_pack_close(ImePack *pack);
const char *ime_pack_path(const ImePack *pack);
/* Per-handle counters: physical reads and logical record accesses (including validation). */
unsigned int ime_pack_io_reads(const ImePack *pack);
unsigned int ime_pack_record_reads(const ImePack *pack);
unsigned int ime_pack_count(const ImePack *pack, int kind);
unsigned int ime_pack_revision(const ImePack *pack);
/* Local index within a kind; does not expose mapped memory or borrowed file buffers. */
int ime_pack_read(ImePack *pack, int kind, unsigned int index, ImePackEntry *out);
/* Worker only, serialized installer. Copies to current_path.install then validates and
 * atomically replaces current_path. On any failure current_path remains untouched.
 * Destination parent must exist. No network IO and no activation in this function. */
int ime_pack_install(const char *download_path, const char *current_path);
#endif
