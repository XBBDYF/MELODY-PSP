"""Run the real catalog/JSON C implementation with HTTP fixtures and temp disk.

Removing ID/size/manifest validation, publishing before validation, or discarding
.old during recovery must break these tests. HTTP alone is mocked: filesystem,
JSON parsing, staging, and the catalog implementation run unchanged.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "skin_catalog.h"
#include "net.h"
static const char *index_data = "{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"Blue\",\"author\":\"A\",\"version\":\"1\",\"description\":\"D\",\"script\":true,\"preview\":true}]}";
static const char *manifest = "{\"schema\":1,\"id\":\"blue\",\"name\":\"Blue\",\"script\":true}";
static const char *script = "return {}";
static int validate_fail, missing_script, force_expect, expected, requests;
static unsigned char png_data[33] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,240,0,0,0,136,8,6,0,0,0,0,0,0,0};
static int bad_preview, fail_publish, preview_oversize, missing_asset, asset_image_size;
static unsigned char wav_data[46] = {'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,'d','a','t','a',2,0,0,0,0,0};
/* Only rename is fault-injected; every successful operation uses real disk. */
#undef rename
extern int rename(const char *, const char *);
int fixture_rename(const char *from, const char *to) {
    size_t n=strlen(from);
    if(fail_publish && n>4 && !strcmp(from+n-4,".new") && !strstr(to,".png")) {
        fail_publish=0;return -1;
    }
    return rename(from,to);
}
int http_get_expect(void) { return expected; }
int http_get(const char *url, char *out, int max, int *len, int timeout, char *err, int n) {
    const char *data; int size;
    (void)timeout; (void)err; (void)n;
    requests++;
    assert(!strncmp(url, SKIN_CATALOG_BASE, strlen(SKIN_CATALOG_BASE)));
    if (strstr(url,"/assets/")) {
        if(missing_asset)return -1;
        if(strstr(url,".wav")){data=(const char*)wav_data;size=sizeof(wav_data);}
        else if(asset_image_size){size=asset_image_size;if(size>=max)size=max-1;memset(out,0,size);memcpy(out,png_data,sizeof(png_data));out[size]=0;*len=size;expected=asset_image_size;return 0;}
        else {data=(const char*)png_data;size=sizeof(png_data);}
        goto copy;
    }
    if (strstr(url,"index.json")) data=index_data;
    else if (strstr(url,"skin.json")) data=manifest;
    else if (strstr(url,"skin.lua")) { if(missing_script)return -1; data=script; }
    else if (strstr(url,"preview.png")) {
        if(bad_preview)return -1;
        if(preview_oversize){memset(out,0,max-1);*len=max-1;out[*len]=0;expected=SKIN_CATALOG_PREVIEW_MAX+1;return 0;}
        data=(const char*)png_data; size=sizeof(png_data); goto copy;
    } else { assert(0); return -1; }
    size=(int)strlen(data);
copy:
    expected=force_expect?size+1:size;
    if(size>=max){memcpy(out,data,max-1);*len=max-1;out[*len]=0;return 0;}
    memcpy(out,data,size);out[size]=0;*len=size;return 0;
}
int skin_runtime_validate(const char *root,char *err,int n) {
    char path[1024]; FILE *f;
    snprintf(path,sizeof(path),"%s/skin.json",root);f=fopen(path,"rb");assert(f);fclose(f);
    if(validate_fail){snprintf(err,n,"fixture invalid Lua");return -1;}
    return 0;
}
static int exists(const char *path) { FILE *f=fopen(path,"rb");if(!f)return 0;fclose(f);return 1; }
int main(int argc,char **argv) {
    char root[1024],path[1100],old[1100],preview[1024];FILE *f;int before;
    assert(argc==2);
    assert(skin_catalog_refresh()==0);
    assert(skin_catalog_count()==1 && !strcmp(skin_catalog_items()[0].name,"Blue"));
    assert(skin_catalog_items()[0].has_script && skin_catalog_items()[0].has_preview);
    assert(skin_catalog_root("blue",root,sizeof(root))==0);
    snprintf(path,sizeof(path),"%s/skin.json",root);
    if(!strcmp(argv[1],"install")) {
        assert(skin_catalog_install("blue")==0);assert(exists(path));
        assert(skin_catalog_items()[0].installed && skin_catalog_progress()==100);
        assert(skin_catalog_preview_path("blue",preview,sizeof(preview))==0 && exists(preview));
        validate_fail=1;manifest="{\"id\":\"blue\",\"script\":true,\"name\":\"BAD\"}";
        assert(skin_catalog_install("blue")<0);f=fopen(path,"rb");assert(f);
        {char buf[256]={0};fread(buf,1,sizeof(buf)-1,f);assert(!strstr(buf,"BAD"));}fclose(f);
        validate_fail=0;missing_script=1;assert(skin_catalog_install("blue")<0);assert(exists(path));
        missing_script=0;bad_preview=1;assert(skin_catalog_install("blue")==0);assert(exists(path));
        fail_publish=1;assert(skin_catalog_install("blue")<0);assert(exists(path));
        snprintf(old,sizeof(old),"%s.old",root);assert(rename(root,old)==0);
        missing_script=1;assert(skin_catalog_install("blue")<0);assert(exists(path));
        missing_script=0;assert(skin_catalog_install("blue")==0);assert(exists(path));
        f=fopen(path,"wb");assert(f);fputs("{\"schema\":1,\"id\":\"blue\",\"name\":\"Blue4\",\"version\":\"4.0\",\"script\":true}",f);fclose(f);
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"Blue3\",\"version\":\"3.0\",\"script\":true}]}";assert(skin_catalog_refresh()==0);before=requests;assert(skin_catalog_install("blue")<0);assert(requests==before);{char v[24];assert(skin_catalog_local_version("blue",v,sizeof(v))==0&&!strcmp(v,"4.0"));}
    } else if(!strcmp(argv[1],"invalid")) {
        before=requests;assert(skin_catalog_install("../bad")<0);assert(requests==before);
        assert(skin_catalog_root("a/b",root,sizeof(root))<0);
        assert(skin_catalog_root("abcdefghijklmnopqrstuvwxyz1234567",root,sizeof(root))<0);
        manifest="{\"id\":\"other\",\"script\":true}";
        assert(skin_catalog_install("blue")<0);assert(!exists(path));
        manifest="{\"id\":\"blue\",\"script\":false}";
        assert(skin_catalog_install("blue")<0);assert(!exists(path));
        manifest="{\"id\":\"blue\",\"id\":\"blue\",\"script\":true}";
        assert(skin_catalog_install("blue")<0);assert(!exists(path));
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"../evil\",\"name\":\"Bad\"}]}";
        assert(skin_catalog_refresh()<0);assert(skin_catalog_count()==1);
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"B\"},{\"id\":\"blue\",\"name\":\"B\"}]}";
        assert(skin_catalog_refresh()<0);assert(skin_catalog_count()==1);
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"B\"},{\"id\":\"BLUE\",\"name\":\"B\"}]}";
        assert(skin_catalog_refresh()<0);assert(skin_catalog_count()==1);
        index_data="{\"schema\":1,\"skins\":{\"0\":{\"id\":\"blue\",\"name\":\"B\"}}}";
        assert(skin_catalog_refresh()<0);
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"B\",\"script\":{}}]}";
        assert(skin_catalog_refresh()<0);
        index_data="{\"schema\":1,\"skins\":[{}]}";assert(skin_catalog_refresh()<0);
        index_data="{\"schema\":1,\"skins\":[]}";assert(skin_catalog_refresh()==0);assert(!skin_catalog_count());
    } else if(!strcmp(argv[1],"local")) {
        assert(skin_catalog_install("blue")==0);
        index_data="{\"schema\":1,\"skins\":[]}";
        assert(skin_catalog_refresh()==0 && skin_catalog_count()==1);
        assert(skin_catalog_items()[0].installed && !strcmp(skin_catalog_items()[0].id,"blue"));
        index_data="broken";assert(skin_catalog_refresh()<0 && skin_catalog_count()==1);
        assert(skin_catalog_load_local()==0 && skin_catalog_count()==1);
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"New blue\",\"script\":true},{\"id\":\"red\",\"name\":\"Red\"}]}";
        assert(skin_catalog_refresh()==0 && skin_catalog_count()==2);
        {int j,found=0;for(j=0;j<skin_catalog_count();j++)if(!strcmp(skin_catalog_items()[j].id,"blue")){assert(skin_catalog_items()[j].installed && !strcmp(skin_catalog_items()[j].name,"New blue"));found=1;}assert(found);}
    } else if(!strcmp(argv[1],"capacity")) {
        char *doc=malloc(SKIN_CATALOG_JSON_MAX+2);int pos=0,j;
        pos+=sprintf(doc+pos,"{\"schema\":1,\"skins\":[");
        for(j=0;j<24;j++)pos+=sprintf(doc+pos,"%s{\"id\":\"s%d\",\"name\":\"Skin%d\"}",j?",":"",j,j);
        pos+=sprintf(doc+pos,"]}");memset(doc+pos,' ',SKIN_CATALOG_JSON_MAX-pos);doc[SKIN_CATALOG_JSON_MAX]=0;
        index_data=doc;assert(skin_catalog_refresh()==0 && skin_catalog_count()==24);
        doc[SKIN_CATALOG_JSON_MAX]=' ';doc[SKIN_CATALOG_JSON_MAX+1]=0;
        assert(skin_catalog_refresh()<0 && skin_catalog_count()==24);
        pos=0;pos+=sprintf(doc+pos,"{\"schema\":1,\"skins\":[");
        for(j=0;j<25;j++)pos+=sprintf(doc+pos,"%s{\"id\":\"s%d\",\"name\":\"Skin%d\"}",j?",":"",j,j);
        sprintf(doc+pos,"]}");assert(skin_catalog_refresh()<0 && skin_catalog_count()==24);free(doc);
    } else if(!strcmp(argv[1],"assets")) {
        char image[1200],sound[1200];
        manifest="{\"schema\":1,\"id\":\"blue\",\"name\":\"Blue\",\"script\":true,\"assets\":[{\"id\":\"wall\",\"file\":\"assets/wall.png\",\"type\":\"image\"},{\"id\":\"click\",\"file\":\"assets/click.wav\",\"type\":\"sound\"}]}";
        assert(skin_catalog_install("blue")==0);
        snprintf(image,sizeof(image),"%s/assets/wall.png",root);snprintf(sound,sizeof(sound),"%s/assets/click.wav",root);
        assert(exists(image)&&exists(sound));
        missing_asset=1;assert(skin_catalog_install("blue")<0);assert(exists(image)&&exists(sound));missing_asset=0;
        wav_data[20]=3;assert(skin_catalog_install("blue")<0);assert(exists(image)&&exists(sound));wav_data[20]=1;
        png_data[16]=1;assert(skin_catalog_install("blue")<0);assert(exists(image)&&exists(sound));png_data[16]=0;
        assert(skin_catalog_install("blue")==0);assert(exists(image)&&exists(sound));
        fail_publish=1;assert(skin_catalog_install("blue")<0);assert(exists(image)&&exists(sound));
        before=requests;
        manifest="{\"id\":\"blue\",\"script\":true,\"assets\":[{\"id\":\"bad\",\"file\":\"assets/../bad.png\",\"type\":\"image\"}]}";
        assert(skin_catalog_install("blue")<0);assert(requests==before+1);assert(exists(image));
        manifest="{\"id\":\"blue\",\"script\":true,\"assets\":{\"0\":{\"id\":\"bad\",\"file\":\"assets/bad.png\",\"type\":\"image\"}}}";
        assert(skin_catalog_install("blue")<0);
        manifest="{\"id\":\"blue\",\"script\":true,\"assets\":[{\"id\":\"wall\",\"file\":\"assets/wall.png\",\"type\":\"image\"},{\"id\":\"WALL\",\"file\":\"assets/click.wav\",\"type\":\"sound\"}]}";
        assert(skin_catalog_install("blue")<0);
        manifest="{\"id\":\"blue\",\"script\":true}";assert(skin_catalog_install("blue")==0);assert(!exists(image)&&!exists(sound));
    } else if(!strcmp(argv[1],"assetcaps")) {
        char doc[6000];int j,pos;
        for(j=0;j<2;j++){
            int k,count=j==0?12:13;
            pos=sprintf(doc,"{\"id\":\"blue\",\"script\":true,\"assets\":[");
            for(k=0;k<count;k++)pos+=sprintf(doc+pos,"%s{\"id\":\"im%d\",\"file\":\"assets/im%d.png\",\"type\":\"image\"}",k?",":"",k,k);
            sprintf(doc+pos,"]}");manifest=doc;
            if(j==0)assert(skin_catalog_install("blue")==0);
            else assert(skin_catalog_install("blue")<0);
        }
        pos=sprintf(doc,"{\"id\":\"blue\",\"script\":true,\"assets\":[");
        for(j=0;j<9;j++)pos+=sprintf(doc+pos,"%s{\"id\":\"snd%d\",\"file\":\"assets/snd%d.wav\",\"type\":\"sound\"}",j?",":"",j,j);
        sprintf(doc+pos,"]}");manifest=doc;assert(skin_catalog_install("blue")<0);
        pos=sprintf(doc,"{\"id\":\"blue\",\"script\":true,\"assets\":[");
        for(j=0;j<9;j++)pos+=sprintf(doc+pos,"%s{\"id\":\"im%d\",\"file\":\"assets/im%d.png\",\"type\":\"image\"}",j?",":"",j,j);
        sprintf(doc+pos,"]}");manifest=doc;asset_image_size=128*1024;
        assert(skin_catalog_install("blue")<0);assert(skin_catalog_items()[0].installed);
        manifest="{\"id\":\"blue\",\"script\":true,\"assets\":[{\"id\":\"a\",\"file\":\"assets/a.png\",\"type\":\"image\"}]}";
        asset_image_size=128*1024+1;assert(skin_catalog_install("blue")<0);asset_image_size=0;
        manifest="{\"id\":\"blue\",\"script\":true,\"assets\":[{\"id\":\"s\",\"file\":\"assets/s.wav\",\"type\":\"sound\"}]}";
        wav_data[24]=1;wav_data[25]=wav_data[26]=wav_data[27]=0;
        wav_data[28]=2;wav_data[29]=wav_data[30]=wav_data[31]=0;
        assert(skin_catalog_install("blue")<0); /* one frame at 1Hz exceeds350ms */
    } else if(!strcmp(argv[1],"bounds")) {
        force_expect=1;assert(skin_catalog_refresh()<0);assert(skin_catalog_count()==1);force_expect=0;
        script=malloc(SKIN_CATALOG_SCRIPT_MAX+2);memset((char*)script,'a',SKIN_CATALOG_SCRIPT_MAX+1);((char*)script)[SKIN_CATALOG_SCRIPT_MAX+1]=0;
        assert(skin_catalog_install("blue")<0);assert(!exists(path));free((void*)script);
        assert(skin_catalog_fetch_preview("blue")==0);
        png_data[16]=1;assert(skin_catalog_fetch_preview("blue")<0);
        assert(skin_catalog_preview_path("blue",preview,sizeof(preview))==0 && exists(preview));
        png_data[16]=0;png_data[0]=0;assert(skin_catalog_fetch_preview("blue")<0);
        png_data[0]=137;preview_oversize=1;assert(skin_catalog_fetch_preview("blue")<0);
        assert(skin_catalog_preview_path("blue",preview,sizeof(preview))==0 && exists(preview));
    } else if(!strcmp(argv[1],"installerror")) {
        /* The skin page must be able to explain a failed switch after the preview
         * and catalog requests that follow have cleared the transient error. */
        assert(skin_catalog_install("blue")==0);
        assert(!skin_catalog_install_error()[0]);
        missing_script=1;
        assert(skin_catalog_install("blue")<0);
        assert(skin_catalog_install_error()[0]);
        missing_script=0;
        index_data="{\"schema\":1,\"skins\":[{\"id\":\"blue\",\"name\":\"Blue\",\"author\":\"A\",\"version\":\"1\",\"description\":\"D\",\"script\":true,\"preview\":true}]}";
        assert(skin_catalog_refresh()==0);
        assert(skin_catalog_install_error()[0]);
        assert(skin_catalog_fetch_preview("blue")==0);
        assert(skin_catalog_install_error()[0]);
        assert(skin_catalog_install("blue")==0);
        assert(!skin_catalog_install_error()[0]);
    }
    return 0;
}
'''


class SkinCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="skin-catalog-")
        cls.directory = Path(cls.temp.name)
        source = cls.directory / "catalog_harness.c"
        source.write_text(HARNESS, encoding="utf-8")
        cls.exe = cls.directory / "catalog_test.exe"
        command = ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                   "-DSKIN_HOST_TEST", "-Drename=fixture_rename", '-DAPP_DIR="' + cls.directory.as_posix() + '/app/"',
                   "-I", str(ROOT / "src"), str(source),
                   str(ROOT / "src/skin_catalog.c"), str(ROOT / "src/json.c"),
                   "-o", str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        app = self.directory / "app"
        if app.exists():
            shutil.rmtree(app)
        app.mkdir()
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_install_validation_rollback_recovery_and_optional_preview(self):
        self.run_case("install")

    def test_reject_traversal_duplicate_wrong_id_and_non_array_catalog(self):
        self.run_case("invalid")

    def test_local_discovery_and_remote_merge_preserve_installed_skins(self):
        self.run_case("local")

    def test_exact_24_skin_and_24kb_json_caps_and_one_over(self):
        self.run_case("capacity")

    def test_required_assets_validate_install_cleanup_and_rollback(self):
        self.run_case("assets")

    def test_asset_image_sound_count_size_total_and_duration_limits(self):
        self.run_case("assetcaps")

    def test_truncated_oversize_and_invalid_png_preserve_prior_files(self):
        self.run_case("bounds")

    def test_failed_install_reason_survives_later_catalog_and_preview_requests(self):
        self.run_case("installerror")


if __name__ == "__main__":
    unittest.main()
