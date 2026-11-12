/* System services contracts: dialogs (progress bars run until closed), PlayGo, trophies. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_services.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int gow3gpu_text_input_begin(const char *text, const char *prompt) { (void)text; (void)prompt; return 0; }
int gow3gpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 0; }
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int64_t runtime_file_read(int fd, void *buffer, uint64_t size) { (void)fd; (void)buffer; (void)size; return -1; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }
static int choice_next=-1;
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus) { (void)title; (void)items; (void)images; (void)focus; return count>0; }
int gow3gpu_choice_poll(void) { return choice_next; }
int runtime_savedata_param(int32_t user, const char *dir, void *param) { (void)user; (void)dir; (void)param; return -1; }
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) { (void)user; (void)dir; (void)out; (void)size; return -1; }
static int trophy_lists_shown, trophies_shown, trophy_last_shown=-1;
void gow3gpu_trophy_list(const char *pack, int list, const uint32_t earned[4]) { (void)pack; (void)list; (void)earned; ++trophy_lists_shown; }
void gow3gpu_trophy_earned(int id) { ++trophies_shown; trophy_last_shown=id; }
static char test_user[64];
const char *runtime_file_user_dir(void) { return test_user; }
int runtime_file_translate(const char *guest, char *out, size_t size) { snprintf(out,size,"%s",guest); return 0; }

int main(void) {
    unsigned char param[128]={0}, result[64]={0}; /* the game zeroes the pointers it does not want */
    uint32_t value;

    /* MsgDialog: a user message finishes with OK; a progress bar runs until closed. */
    assert(msg_init()==0);
    memset(param+56,0,4); param[56]=1;
    assert(msg_open(param)==0 && msg_status()==DIALOG_FINISHED);
    assert(msg_result(result)==0);
    memcpy(&value,result+4,4); assert(value==1);
    param[56]=2;
    assert(msg_open(param)==0 && msg_status()==DIALOG_RUNNING);
    assert(msg_result(result)==DIALOG_NOT_FINISHED);
    assert(msg_progress(0,50)==0 && msg_progress(1,50)==DIALOG_PARAM_INVALID);
    assert(msg_close()==0 && msg_status()==DIALOG_FINISHED && msg_close()==DIALOG_NOT_RUNNING);
    assert(msg_progress(0,100)==DIALOG_NOT_RUNNING);
    assert(msg_result(NULL)==DIALOG_ARG_NULL);
    assert(msg_term()==0 && msg_open(NULL)==DIALOG_ARG_NULL);

    /* SaveDataDialog: mode at offset 52 (5 = progress bar); result after the mode field. */
    assert(save_init()==0 && save_ready()==1);
    param[52]=5;
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING && save_progress(0,10)==0);
    assert(save_close()==0 && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==0); /* closed by the game: no button */
    param[52]=2;
    assert(save_open(param)==0 && save_status()==DIALOG_FINISHED && save_result(result)==0);
    memcpy(&value,result+8,4); assert(value==1);
    assert(save_term()==0);

    /* Save list: the player's choice comes back as the save directory. */
    char names[2][32]={"SAVE0001","SAVE0002"}, chosen[32];
    unsigned char items[64]={0}, list[128]={0}, out[64]={0};
    const char *names_ptr=names[0]; uint32_t name_count=2; const char *new_title="New";
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&name_count,4); memcpy(items+32,&new_title,8);
    unsigned char *items_ptr=items; char *chosen_ptr=chosen;
    list[52]=1; list[56]=2; memcpy(list+72,&items_ptr,8);
    memcpy(out+16,&chosen_ptr,8);
    assert(save_init()==0 && save_open(list)==0 && save_status()==DIALOG_RUNNING);
    choice_next=2; /* entries: New save, SAVE0001, SAVE0002 */
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && !strcmp(chosen,"SAVE0002"));
    memcpy(&value,out+4,4); assert(value==0);
    memcpy(&value,out+8,4); assert(value==1);
    assert(save_open(list)==0);
    choice_next=0;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0 && chosen[0]==0); /* new save */
    assert(save_open(list)==0);
    choice_next=-2;
    assert(save_status()==DIALOG_FINISHED && save_result(out)==0);
    memcpy(&value,out+4,4); assert(value==SAVE_RESULT_CANCELED);
    assert(save_term()==0);

    /* PlayGo: installed, full speed, progress never divides by zero. */
    int32_t handle=0; uint64_t progress[2]; int32_t speed; uint16_t chunk=0;
    assert(playgo_init(NULL)==0 && playgo_open(&handle,NULL)==0);
    assert(playgo_get_speed(handle,&speed)==0 && speed==2);
    assert(playgo_progress(handle,&chunk,1,progress)==0 && progress[0]==progress[1] && progress[1]);
    assert(playgo_progress(handle,&chunk,0,progress)==PLAYGO_BAD_SIZE);
    assert(playgo_todo(handle,&chunk,1)==0 && playgo_todo(handle,NULL,1)==PLAYGO_BAD_POINTER);
    uint64_t mask=0;
    assert(playgo_get_language(handle,&mask)==0 && mask==(UINT64_C(1)<<62));
    assert(playgo_set_language(handle,UINT64_C(1)<<55)==0);
    assert(playgo_get_language(handle,&mask)==0 && mask==(UINT64_C(1)<<55));
    assert(playgo_get_language(handle,NULL)==PLAYGO_BAD_POINTER);
    int64_t eta=-1;
    assert(playgo_eta(handle,&chunk,1,&eta)==0 && eta==0);
    uint16_t invalid_chunk=100;
    eta=42;
    assert(playgo_eta(handle,&invalid_chunk,1,&eta)==PLAYGO_BAD_CHUNK && eta==42);
    assert(playgo_prefetch(handle,&chunk,1,3)==0);
    assert(playgo_prefetch(handle,&invalid_chunk,1,3)==PLAYGO_BAD_CHUNK);
    uint32_t pending=7; unsigned char untouched[4]={1,2,3,4};
    assert(playgo_get_todo(handle,untouched,1,&pending)==0 && pending==0 && untouched[0]==1);
    assert(playgo_get_todo(handle,untouched,0,&pending)==PLAYGO_BAD_SIZE);
    assert(playgo_close(handle)==0 && playgo_close(handle)==PLAYGO_BAD_HANDLE && playgo_terminate()==0);

    /* Trophies: none until earned; an earned one is saved, reported back and shown once. */
    snprintf(test_user,sizeof(test_user),"services-test-user-%d",(int)getpid());
    mkdir(test_user,0755);
    uint32_t flags[4]={~0u,~0u,~0u,~0u}, count=7;
    assert(trophy_unlock_state(1,1,flags,&count)==0 && !flags[0] && !flags[3] && count==0);
    assert(trophy_unlock_state(1,1,NULL,&count)==TROPHY_INVALID && trophy_release(0)==TROPHY_INVALID);
    int32_t trophy_ctx=0, trophy_hd=0, platinum=0;
    /* A label without a list keeps nothing. */
    int32_t no_list_ctx=0;
    assert(trophy_context(&no_list_ctx,1,9,0)==0 && trophy_unlock(no_list_ctx,1,3,&platinum)==0 && trophies_shown==0);
    /* The Last of Us Remastered: service label 0, trophy00.trp. */
    assert(trophy_context(&trophy_ctx,1,0,0)==0 && trophy_handle(&trophy_hd)==0);
    assert(trophy_register(trophy_ctx,trophy_hd,0)==0);
    assert(trophy_unlock(trophy_ctx,trophy_hd,7,&platinum)==0 && trophies_shown==1 && trophy_last_shown==7);
    assert(trophy_unlock_state(trophy_ctx,trophy_hd,flags,&count)==0 && flags[0]==1u<<7 && count==1);
    trophies_shown=0;
    { char p0[128]; trophy_file(0,p0,sizeof(p0)); remove(p0); }
    assert(trophy_context(&trophy_ctx,1,2,0)==0 && trophy_handle(&trophy_hd)==0);
    trophy_lists_shown=0;
    assert(trophy_register(trophy_ctx,trophy_hd,0)==0 && trophy_lists_shown==1);
    assert(trophy_unlock(trophy_ctx,trophy_hd,5,&platinum)==0 && platinum==-1 && trophies_shown==1 && trophy_last_shown==5);
    assert(trophy_unlock(trophy_ctx,trophy_hd,5,&platinum)==0 && trophies_shown==1);
    assert(trophy_unlock(trophy_ctx,trophy_hd,128,&platinum)==TROPHY_INVALID);
    assert(trophy_unlock_state(trophy_ctx,trophy_hd,flags,&count)==0 && flags[0]==1u<<5 && !flags[1] && count==1);
    /* The next start reads it back. */
    memset(trophy_flags,0,sizeof(trophy_flags)); memset(trophy_loaded,0,sizeof(trophy_loaded));
    memset(trophy_contexts,0,sizeof(trophy_contexts));
    assert(trophy_context(&trophy_ctx,1,2,0)==0);
    assert(trophy_unlock_state(trophy_ctx,trophy_hd,flags,&count)==0 && flags[0]==1u<<5 && count==1);
    char trophy_path[128];
    trophy_file(2,trophy_path,sizeof(trophy_path)); remove(trophy_path);
    snprintf(trophy_path,sizeof(trophy_path),"%s/trophy",test_user); rmdir(trophy_path); rmdir(test_user);

    /* PSN: signed out by default; GOW3_NP_SIGNED_IN pretends a sign-in, with no transfers. */
    int32_t state=-1, polled=-1; unsigned char npid[36]; uint64_t account=0;
    np_signed_in_mode=0;
    assert(np_state(1,&state)==0 && state==1 && np_get_np_id(1,npid)==NP_SIGNED_OUT);
    assert(netctl_state(&state)==0 && state==0 && netctl_info(14,npid)==NET_CTL_NOT_CONNECTED);
    assert(np_poll(1,&polled)==0 && polled==NP_SIGNED_OUT);
    np_signed_in_mode=1;
    assert(np_state(1,&state)==0 && state==2 && netctl_state(&state)==0 && state==3);
    memset(npid,0xff,sizeof(npid));
    assert(np_get_np_id(1,npid)==0 && !strcmp((char *)npid,"Hunter") && npid[16]==0 && npid[35]==0);
    assert(np_get_account_id(1,&account)==0 && account!=0);
    assert(netctl_info(14,npid)==0 && !strcmp((char *)npid,"10.0.0.2"));
    assert(np_poll(1,&polled)==0 && polled==0);
    assert(net_unreachable()==NET_ENETUNREACH && http_fail()==HTTP_NETWORK); /* still no transfers */
    np_signed_in_mode=0;

    /* URI parsing (no network): size query first, then the parts copied into the pool. */
    uint64_t need=0; unsigned char element[80]; char uri_pool[256]; char *part;
    const char *uri="http://t1.campaign.config.s3.amazonaws.com/campaign3.config.txt.crypt";
    assert(http_uri_parse(element,uri,NULL,&need,0)==0 && need>strlen(uri));
    assert(http_uri_parse(element,uri,uri_pool,&need,need-1)==HTTP_OUT_OF_MEMORY);
    assert(http_uri_parse(element,uri,uri_pool,&need,need)==0);
    memcpy(&part,element+8,8); assert(!strcmp(part,"http"));
    memcpy(&part,element+32,8); assert(!strcmp(part,"t1.campaign.config.s3.amazonaws.com"));
    memcpy(&part,element+40,8); assert(!strcmp(part,"/campaign3.config.txt.crypt"));
    memcpy(&part,element+48,8); assert(!strcmp(part,""));
    uint16_t port; memcpy(&port,element+64,2); assert(port==80);
    assert(http_uri_parse(element,"https://user:pw@host:8443/a/b?x=1#f",uri_pool,&need,sizeof(uri_pool))==0);
    memcpy(&part,element+16,8); assert(!strcmp(part,"user"));
    memcpy(&part,element+24,8); assert(!strcmp(part,"pw"));
    memcpy(&part,element+32,8); assert(!strcmp(part,"host"));
    memcpy(&part,element+48,8); assert(!strcmp(part,"?x=1"));
    memcpy(&part,element+56,8); assert(!strcmp(part,"#f"));
    memcpy(&port,element+64,2); assert(port==8443);
    char built[128]; uint64_t built_need=0;
    assert(http_uri_build(built,&built_need,sizeof(built),element,0xffff)==0);
    assert(!strcmp(built,"https://user:pw@host:8443/a/b?x=1#f") && built_need==strlen(built)+1);
    assert(http_uri_build(built,NULL,4,element,0xffff)==HTTP_OUT_OF_MEMORY);
    assert(http_uri_parse(element,uri,uri_pool,&need,sizeof(uri_pool))==0);
    assert(http_uri_build(built,NULL,sizeof(built),element,0xffff)==0 && !strcmp(built,uri)); /* port 80 left out */

    float ratio=0; assert(safe_area(&ratio)==0 && ratio==1.0f && safe_area(NULL)==SYSTEM_PARAMETER);
    puts("services test: ok");
    return 0;
}
