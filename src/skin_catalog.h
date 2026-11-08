#ifndef MELODY_SKIN_CATALOG_H
#define MELODY_SKIN_CATALOG_H

#define SKIN_CATALOG_MAX 24
#define SKIN_CATALOG_JSON_MAX (24 * 1024)
#define SKIN_CATALOG_SCRIPT_MAX (48 * 1024)
#define SKIN_CATALOG_PREVIEW_MAX (128 * 1024)
#define SKIN_CATALOG_BASE "http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/skins/"

typedef struct {
    char id[33];
    char name[64];
    char author[64];
    char version[24];
    char description[192];
    int installed;
    int has_script;
    int has_preview; /* Advertised asset; use preview_path to check cached availability. */
} SkinCatalogEntry;

enum {
    SKIN_CATALOG_IDLE = 0,
    SKIN_CATALOG_REFRESHING,
    SKIN_CATALOG_INSTALLING,
    SKIN_CATALOG_FETCHING_PREVIEW,
    SKIN_CATALOG_READY,
    SKIN_CATALOG_ERROR
};

/* Synchronous: ONLY call from the existing serialized API HTTP worker.
 * No thread creation, cancellation ownership, or active-skin apply.
 * Installation delegates candidate checks to skin_runtime_validate (dry runtime).
 * skin.json may declare assets:[{id,file,type}], max20 (12image/8sound).
 * file is assets/<ASCII-safe-id>.png/.wav; image <=128KB and512x512;
 * sound <=64KB, PCM16 mono/stereo <=44100Hz, <=350ms; total assets <=1MB.
 * Declared assets are required and staged before validation and publication.
 * Return 0 on success, -1 on error; failed refresh preserves the last catalog.
 * API/UI must serialize list/root reads with mutations (state/progress may poll).
 */
/* Discover bundled/installed skin.json files without executing Lua/network IO.
 * Call only while the API worker is idle; remote metadata is retained by ID.
 */
int skin_catalog_load_local(void);
int skin_catalog_refresh(void);
int skin_catalog_install(const char *id);
int skin_catalog_install_id(const char *id);
/* Optional preview cache, independent of installed skins; same worker rules.
 * Cache path getter returns -1 when missing; output is always cleared on failure.
 */
int skin_catalog_fetch_preview(const char *id);
int skin_catalog_preview_path(const char *id, char *out, int n);
const SkinCatalogEntry *skin_catalog_items(void);
int skin_catalog_count(void);
int skin_catalog_local_version(const char*id,char*out,int cap);
int skin_catalog_state(void);
int skin_catalog_progress(void); /* 0..100 */
const char *skin_catalog_error(void);
/* Survives the preview/catalog requests that follow a failed install, so the
 * skin page can keep showing the reason the selected theme did not activate. */
const char *skin_catalog_install_error(void);
/* Checked deterministic installed root; no trailing slash. Does not require install. */
int skin_catalog_root(const char *id, char *out, int n);

#endif
