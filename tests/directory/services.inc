/* Minimal deterministic Windows services used by ordinary enumeration fixtures. */
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xc000000f)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define FILE_LIST_DIRECTORY 1
#define UserMode 1
#define SL_INDEX_SPECIFIED 4
#define SL_RESTART_SCAN 1
#define SL_RETURN_SINGLE_ENTRY 2
#define PagedPool 1
#define ALLOC_TAG 0
#define NormalPagePriority 0
static void* ExAllocatePoolWithTag(int pool,size_t size,int tag){(void)pool;(void)tag;return malloc(size);}
static void ExFreePool(void* p){free(p);}
static void RtlFreeUnicodeString(UNICODE_STRING* s){free(s->Buffer);s->Buffer=NULL;s->Length=s->MaximumLength=0;}
static WCHAR uppercase(WCHAR c){return c>='a'&&c<='z'?c-'a'+'A':c;}
static NTSTATUS RtlUpcaseUnicodeString(UNICODE_STRING* out,const UNICODE_STRING* in,bool allocate){
    if(allocate){out->Buffer=malloc(in->Length);if(!out->Buffer)return STATUS_INSUFFICIENT_RESOURCES;}
    out->Length=out->MaximumLength=in->Length;
    for(unsigned n=0;n<in->Length/2;n++)out->Buffer[n]=uppercase(in->Buffer[n]);
    return STATUS_SUCCESS;
}
static bool FsRtlDoesNameContainWildCards(const UNICODE_STRING* s){for(unsigned n=0;n<s->Length/2;n++)if(s->Buffer[n]=='*'||s->Buffer[n]=='?')return true;return false;}
static bool match(const WCHAR* p,unsigned pn,const WCHAR* n,unsigned nn,bool insensitive){
    if(!pn)return !nn;
    if(*p=='*')return match(p+1,pn-1,n,nn,insensitive)||(nn&&match(p,pn,n+1,nn-1,insensitive));
    if(!nn)return false;
    return (*p=='?'||(insensitive?uppercase(*p)==uppercase(*n):*p==*n))&&match(p+1,pn-1,n+1,nn-1,insensitive);
}
static bool FsRtlIsNameInExpression(const UNICODE_STRING* p,const UNICODE_STRING* n,bool insensitive,void* table){(void)table;return match(p->Buffer,p->Length/2,n->Buffer,n->Length/2,insensitive);}
static size_t RtlCompareMemory(const void* a,const void* b,size_t length){size_t n=0;while(n<length&&((const char*)a)[n]==((const char*)b)[n])n++;return n;}
static uint32_t calc_crc32c(uint32_t crc,const uint8_t* p,size_t size){while(size--)crc=(crc*33)^*p++;return crc;}
static void ExAcquireResourceSharedLite(ERESOURCE* r,bool wait){(void)r;(void)wait;}
static void ExReleaseResourceLite(ERESOURCE* r){(void)r;}
static void* map_user_buffer(PIRP irp,int priority){(void)priority;return irp->buffer;}
static void init_list(LIST_ENTRY* l){l->Flink=l->Blink=l;}
static void append(LIST_ENTRY* head,LIST_ENTRY* l){l->Flink=head;l->Blink=head->Blink;head->Blink->Flink=l;head->Blink=l;}
