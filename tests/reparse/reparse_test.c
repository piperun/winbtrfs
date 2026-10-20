/* Exact reparse deletion body; persistence and kernel services are modeled. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS;
typedef uint32_t ULONG, DWORD;
typedef struct { int unused; } LIST_ENTRY;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { uint8_t bytes[16]; } GUID;
struct btrfs_timespec { uint64_t sec; };
typedef struct { uint16_t Length, MaximumLength; void* Buffer; } ANSI_STRING;
typedef struct { uint32_t ReparseTag; uint16_t ReparseDataLength, Reserved; struct { char DataBuffer[1]; } GenericReparseBuffer; } REPARSE_DATA_BUFFER;
typedef struct { uint32_t ReparseTag; uint16_t ReparseDataLength, Reserved; GUID ReparseGuid; struct { char DataBuffer[1]; } GenericReparseBuffer; } REPARSE_GUID_DATA_BUFFER;
typedef struct { int tree_lock; struct { uint64_t generation; } superblock; } device_extension;
typedef struct { struct { uint64_t ctransid; struct btrfs_timespec ctime; } root_item; } root;
typedef struct { unsigned type; bool ads, atts_changed, inode_item_changed, reparse_xattr_changed, fail_read; ULONG atts; struct { int* Resource; } Header; device_extension* Vcb; root* subvol; struct { uint64_t size, generation, transid, sequence; ULONG mode; struct btrfs_timespec ctime,mtime; } inode_item; ANSI_STRING reparse_xattr; uint8_t data[32]; } fcb;
typedef struct { unsigned type; } dir_child;
typedef struct { fcb* fcb; dir_child* dc; } file_ref;
typedef struct { unsigned access; bool user_set_change_time, user_set_write_time, lxss; file_ref* fileref; } ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { PFILE_OBJECT FileObject; struct { struct { DWORD InputBufferLength; } DeviceIoControl; } Parameters; } IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct { IO_STACK_LOCATION* stack; struct { void* SystemBuffer; } AssociatedIrp; unsigned RequestorMode; } IRP, *PIRP;
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2
#define BTRFS_FT_SYMLINK 7
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_WRITE_ATTRIBUTES 0x100
#define IO_REPARSE_TAG_SYMLINK 0xa000000cU
#define IO_REPARSE_TAG_LX_SYMLINK 0xa000001dU
#define IO_REPARSE_TAG_MOUNT_POINT 0xa0000003U
#define IO_REPARSE_TAG_FAKE_MICROSOFT 0x8000f001U
#define IO_REPARSE_TAG_FAKE_THIRD_PARTY 0x0000f001U
#define REPARSE_DATA_BUFFER_HEADER_SIZE 8
#define REPARSE_GUID_DATA_BUFFER_HEADER_SIZE 24
#define IsReparseTagMicrosoft(tag) (((tag) & 0x80000000U) != 0)
#define IO_REPARSE_TAG_VALID_VALUES 0xF000FFFFU
#define IO_REPARSE_TAG_RESERVED_RANGE 1
#define IsReparseTagValid(tag) (!((tag) & ~IO_REPARSE_TAG_VALID_VALUES) && ((tag) > IO_REPARSE_TAG_RESERVED_RANGE))
#define __S_IFLNK 0120000
#define __S_IFREG 0100000
#define UserMode 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_NOT_A_REPARSE_POINT ((NTSTATUS)0xc0000275)
#define STATUS_IO_REPARSE_TAG_INVALID ((NTSTATUS)0xc0000276)
#define STATUS_IO_REPARSE_TAG_MISMATCH ((NTSTATUS)0xc0000277)
#define STATUS_IO_REPARSE_DATA_INVALID ((NTSTATUS)0xc0000278)
#define STATUS_REPARSE_ATTRIBUTE_CONFLICT ((NTSTATUS)0xc00002b2)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define NT_SUCCESS(s) ((s)>=0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define FILE_NOTIFY_CHANGE_LAST_WRITE 1
#define FILE_NOTIFY_CHANGE_ATTRIBUTES 2
#define FILE_ACTION_MODIFIED 3
static unsigned truncates, notifications, locks, reads;
static PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP i) { return i->stack; }
static void InitializeListHead(LIST_ENTRY* l) { (void)l; }
static void ExAcquireResourceSharedLite(int* r, bool w) { (void)r; (void)w; locks++; }
static void ExAcquireResourceExclusiveLite(int* r, bool w) { (void)r; (void)w; locks++; }
static void ExReleaseResourceLite(int* r) { (void)r; locks--; }
static void KeQuerySystemTime(LARGE_INTEGER* t) { t->QuadPart=42; }
static void win_time_to_unix(LARGE_INTEGER t, struct btrfs_timespec* n) { n->sec=t.QuadPart; }
static void mark_fileref_dirty(file_ref* r) { (void)r; }
static void mark_fcb_dirty(fcb* f) { (void)f; }
static NTSTATUS truncate_file(fcb* f, uint64_t n, PIRP i, LIST_ENTRY* r) { (void)i; (void)r; truncates++; f->inode_item.size=n; return 0; }
static inline NTSTATUS read_file(fcb* f, uint8_t* data, uint64_t start, uint64_t length, ULONG* pbr, PIRP i) {
 (void)i; reads++; if (pbr) *pbr=0;
 if (f->fail_read) return STATUS_UNSUCCESSFUL;
 if (start >= f->inode_item.size) return STATUS_UNSUCCESSFUL;
 if (length > f->inode_item.size-start) length=f->inode_item.size-start;
 memcpy(data, f->data+start, (size_t)length);
 if (pbr) *pbr=(ULONG)length;
 return STATUS_SUCCESS;
}
static void ExFreePool(void* p) { free(p); }
static inline void RtlCopyMemory(void* dest, const void* source, size_t len) { memcpy(dest,source,len); }
static inline size_t RtlCompareMemory(const void* a, const void* b, size_t len) {
 const uint8_t *x=a, *y=b; size_t i=0; while (i<len && x[i]==y[i]) i++; return i;
}
static void clear_rollback(LIST_ENTRY* l) { (void)l; }
static void do_rollback(device_extension* v, LIST_ENTRY* l) { (void)v; (void)l; }
static void queue_notification_fcb(file_ref* r, unsigned m, unsigned a, void* n) { (void)r; (void)m; (void)a; (void)n; notifications++; }
#include "delete.inc"

typedef struct {
 const char* name;
 unsigned type;
 bool has_reparse, lxss, wrong_guid, fail_read;
 ULONG stored_tag, request_tag;
 DWORD input_length;
 unsigned stored_length;
 NTSTATUS expected;
 unsigned request_data_length;
} scenario;

static bool run_scenario(const scenario* s) {
 device_extension v={.superblock.generation=12}; root sub={0}; int resource=0;
 fcb f={.type=s->type,.atts=(s->has_reparse?FILE_ATTRIBUTE_REPARSE_POINT:0)|0x20,
        .Header.Resource=&resource,.Vcb=&v,.subvol=&sub,.fail_read=s->fail_read,
        .inode_item.size=s->stored_length?s->stored_length:32,.inode_item.mode=__S_IFLNK,
        .inode_item.generation=4,.inode_item.transid=5,.inode_item.sequence=6,
        .inode_item.ctime.sec=7,.inode_item.mtime.sec=8};
 sub.root_item.ctransid=9; sub.root_item.ctime.sec=10;
 dir_child dc={.type=f.type}; file_ref ref={.fcb=&f,.dc=&dc};
 ccb cb={.access=FILE_WRITE_ATTRIBUTES,.lxss=s->lxss,.fileref=&ref}; FILE_OBJECT fo={&f,&cb};
 REPARSE_GUID_DATA_BUFFER stored={.ReparseTag=s->stored_tag,.ReparseDataLength=8};
 REPARSE_GUID_DATA_BUFFER input={.ReparseTag=s->request_tag,.ReparseDataLength=(uint16_t)s->request_data_length};
 for (unsigned i=0;i<sizeof(GUID);i++) stored.ReparseGuid.bytes[i]=(uint8_t)(i+1);
 input.ReparseGuid=stored.ReparseGuid;
 if (s->wrong_guid) input.ReparseGuid.bytes[0]++;
 memcpy(f.data,&stored,sizeof(stored));
 if (f.type==BTRFS_FT_DIR) {
  f.reparse_xattr.Buffer=malloc(sizeof(f.data));
  if (!f.reparse_xattr.Buffer) return false;
  memcpy(f.reparse_xattr.Buffer,f.data,sizeof(f.data));
  f.reparse_xattr.Length=(uint16_t)f.inode_item.size;
 }
 uint8_t original_data[sizeof(f.data)]; memcpy(original_data,f.data,sizeof(original_data));
 uint8_t original_inode[sizeof(f.inode_item)]; memcpy(original_inode,&f.inode_item,sizeof(original_inode));
 uint8_t original_root[sizeof(sub.root_item)]; memcpy(original_root,&sub.root_item,sizeof(original_root));
 void* original_xattr=f.reparse_xattr.Buffer;
 ULONG original_atts=f.atts; uint64_t original_size=f.inode_item.size;
 IO_STACK_LOCATION sp={.FileObject=&fo,.Parameters.DeviceIoControl.InputBufferLength=s->input_length};
 IRP irp={.stack=&sp,.AssociatedIrp.SystemBuffer=&input,.RequestorMode=UserMode};
 truncates=notifications=locks=reads=0;
 NTSTATUS status=delete_reparse_point(&irp);
 bool ok=status==s->expected && locks==0;
 if (s->expected==STATUS_SUCCESS) {
  ok=ok && !(f.atts&FILE_ATTRIBUTE_REPARSE_POINT) && f.atts_changed && notifications==1;
  ok=ok && (truncates==(s->type==BTRFS_FT_REG_FILE));
  if (s->type==BTRFS_FT_REG_FILE) ok=ok && f.inode_item.size==0;
  if (s->type==BTRFS_FT_DIR) ok=ok && f.reparse_xattr.Buffer==NULL;
  if (s->type==BTRFS_FT_SYMLINK) ok=ok && f.type==BTRFS_FT_REG_FILE && dc.type==BTRFS_FT_REG_FILE;
 } else {
  ok=ok && f.type==s->type && f.atts==original_atts && f.inode_item.size==original_size;
  ok=ok && !f.atts_changed && !f.inode_item_changed && !f.reparse_xattr_changed;
  ok=ok && !truncates && !notifications && f.reparse_xattr.Buffer==original_xattr;
  ok=ok && memcmp(f.data,original_data,sizeof(f.data))==0;
  ok=ok && memcmp(&f.inode_item,original_inode,sizeof(original_inode))==0;
  ok=ok && memcmp(&sub.root_item,original_root,sizeof(original_root))==0;
  if (original_xattr) ok=ok && memcmp(original_xattr,original_data,sizeof(f.data))==0;
 }
 printf("%s %-34s status=%08x reads=%u truncates=%u notifications=%u\n",
        ok?"PASS":"FAIL",s->name,(unsigned)status,reads,truncates,notifications);
 if (f.reparse_xattr.Buffer) free(f.reparse_xattr.Buffer);
 return ok;
}

int main(void) {
 static const scenario cases[]={
  {"native symlink match",BTRFS_FT_SYMLINK,true,false,false,false,IO_REPARSE_TAG_SYMLINK,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_SUCCESS,0},
  {"native symlink tag mismatch",BTRFS_FT_SYMLINK,true,false,false,false,IO_REPARSE_TAG_SYMLINK,IO_REPARSE_TAG_MOUNT_POINT,8,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"LX symlink match",BTRFS_FT_SYMLINK,true,true,false,false,IO_REPARSE_TAG_LX_SYMLINK,IO_REPARSE_TAG_LX_SYMLINK,8,0,STATUS_SUCCESS,0},
  {"LX symlink tag mismatch",BTRFS_FT_SYMLINK,true,true,false,false,IO_REPARSE_TAG_LX_SYMLINK,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"plain file",BTRFS_FT_REG_FILE,false,false,false,false,IO_REPARSE_TAG_SYMLINK,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_NOT_A_REPARSE_POINT,0},
  {"plain directory",BTRFS_FT_DIR,false,false,false,false,IO_REPARSE_TAG_SYMLINK,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_NOT_A_REPARSE_POINT,0},
  {"file Microsoft tag match",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,8,0,STATUS_SUCCESS,0},
  {"file Microsoft GUID header",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,24,0,STATUS_SUCCESS,0},
  {"file Microsoft tag mismatch",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"directory Microsoft tag match",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,8,0,STATUS_SUCCESS,0},
  {"directory Microsoft GUID header",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,24,0,STATUS_SUCCESS,0},
  {"directory Microsoft tag mismatch",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_SYMLINK,8,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"file GUID match",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,0,STATUS_SUCCESS,0},
  {"file GUID tag mismatch",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY+1,24,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"file GUID mismatch",BTRFS_FT_REG_FILE,true,false,true,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,0,STATUS_REPARSE_ATTRIBUTE_CONFLICT,0},
  {"directory GUID match",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,0,STATUS_SUCCESS,0},
  {"directory GUID tag mismatch",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY+1,24,0,STATUS_IO_REPARSE_TAG_MISMATCH,0},
  {"directory GUID mismatch",BTRFS_FT_DIR,true,false,true,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,0,STATUS_REPARSE_ATTRIBUTE_CONFLICT,0},
  {"file GUID header missing",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,8,0,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"directory GUID header missing",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,8,0,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"Microsoft header short",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,7,0,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"Microsoft header extra",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,9,0,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"delete data nonzero",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,8,0,STATUS_IO_REPARSE_DATA_INVALID,1},
  {"file stored header short",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,7,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"directory stored header short",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,7,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"file stored GUID short",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,12,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"directory stored GUID short",BTRFS_FT_DIR,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,IO_REPARSE_TAG_FAKE_THIRD_PARTY,24,12,STATUS_IO_REPARSE_DATA_INVALID,0},
  {"file read failure",BTRFS_FT_REG_FILE,true,false,false,true,IO_REPARSE_TAG_FAKE_MICROSOFT,IO_REPARSE_TAG_FAKE_MICROSOFT,8,0,STATUS_UNSUCCESSFUL,0},
  {"reserved tag",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_SYMLINK,0,8,0,STATUS_IO_REPARSE_TAG_INVALID,0},
  {"invalid reserved bits",BTRFS_FT_REG_FILE,true,false,false,false,IO_REPARSE_TAG_FAKE_THIRD_PARTY,0x00010003U,24,0,STATUS_IO_REPARSE_TAG_INVALID,0},
 };
 unsigned failed=0;
 for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) failed+=!run_scenario(&cases[i]);
 printf("%zu/%zu reparse scenarios passed\n",sizeof(cases)/sizeof(cases[0])-failed,sizeof(cases)/sizeof(cases[0]));
 return failed!=0;
}
