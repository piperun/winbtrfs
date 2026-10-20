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
struct btrfs_timespec { uint64_t sec; };
typedef struct { uint16_t Length, MaximumLength; void* Buffer; } ANSI_STRING;
typedef struct { uint32_t ReparseTag; uint16_t ReparseDataLength, Reserved; struct { char DataBuffer[1]; } GenericReparseBuffer; } REPARSE_DATA_BUFFER;
typedef struct { int tree_lock; struct { uint64_t generation; } superblock; } device_extension;
typedef struct { struct { uint64_t ctransid; struct btrfs_timespec ctime; } root_item; } root;
typedef struct { unsigned type; bool ads, atts_changed, inode_item_changed, reparse_xattr_changed; ULONG atts; struct { int* Resource; } Header; device_extension* Vcb; root* subvol; struct { uint64_t size, generation, transid, sequence; ULONG mode; struct btrfs_timespec ctime,mtime; } inode_item; ANSI_STRING reparse_xattr; } fcb;
typedef struct { unsigned type; } dir_child;
typedef struct { fcb* fcb; dir_child* dc; } file_ref;
typedef struct { unsigned access; bool user_set_change_time, user_set_write_time; file_ref* fileref; } ccb;
typedef struct { fcb* FsContext; ccb* FsContext2; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct { PFILE_OBJECT FileObject; struct { struct { DWORD InputBufferLength; } DeviceIoControl; } Parameters; } IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct { IO_STACK_LOCATION* stack; struct { void* SystemBuffer; } AssociatedIrp; unsigned RequestorMode; } IRP, *PIRP;
#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2
#define BTRFS_FT_SYMLINK 7
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_WRITE_ATTRIBUTES 0x100
#define IO_REPARSE_TAG_SYMLINK 0xa000000cU
#define __S_IFLNK 0120000
#define __S_IFREG 0100000
#define UserMode 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_NOT_A_REPARSE_POINT ((NTSTATUS)0xc0000275)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define NT_SUCCESS(s) ((s)>=0)
#define TRACE(...) ((void)0)
#define ERR(...) ((void)0)
#define WARN(...) ((void)0)
#define FILE_NOTIFY_CHANGE_LAST_WRITE 1
#define FILE_NOTIFY_CHANGE_ATTRIBUTES 2
#define FILE_ACTION_MODIFIED 3
static unsigned truncates, notifications, locks;
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
static void ExFreePool(void* p) { free(p); }
static void clear_rollback(LIST_ENTRY* l) { (void)l; }
static void do_rollback(device_extension* v, LIST_ENTRY* l) { (void)v; (void)l; }
static void queue_notification_fcb(file_ref* r, unsigned m, unsigned a, void* n) { (void)r; (void)m; (void)a; (void)n; notifications++; }
#include "delete.inc"
int main(void) {
 unsigned failed=0;
 unsigned total=0;
 for (unsigned c=0; c<6; c++) {
  device_extension v={.superblock.generation=12}; root sub={0}; int resource=0;
  unsigned type=c==0||c==5?BTRFS_FT_SYMLINK:c==1||c==3?BTRFS_FT_REG_FILE:BTRFS_FT_DIR;
  fcb f={.type=type,.atts=c==1||c==2?0x20:FILE_ATTRIBUTE_REPARSE_POINT|0x20,.Header.Resource=&resource,.Vcb=&v,.subvol=&sub,.inode_item.size=17,.inode_item.mode=__S_IFLNK};
  dir_child dc={.type=f.type}; file_ref ref={.fcb=&f,.dc=&dc}; ccb cb={.access=FILE_WRITE_ATTRIBUTES,.fileref=&ref}; FILE_OBJECT fo={&f,&cb};
  REPARSE_DATA_BUFFER rdb={.ReparseTag=c==5?0:IO_REPARSE_TAG_SYMLINK}; IO_STACK_LOCATION sp={.FileObject=&fo,.Parameters.DeviceIoControl.InputBufferLength=8}; IRP irp={.stack=&sp,.AssociatedIrp.SystemBuffer=&rdb,.RequestorMode=UserMode};
  ULONG disk_atts=f.atts; truncates=notifications=locks=0;
  NTSTATUS status=delete_reparse_point(&irp);
  if (f.atts_changed) disk_atts=f.atts; /* Exact flushthread.c DOSATTRIB dirty predicate. */
  bool ok;
  if(c==1||c==2||c==5)ok=status==(c==5?STATUS_INVALID_PARAMETER:STATUS_NOT_A_REPARSE_POINT)&&f.inode_item.size==17&&!truncates&&!notifications&&!f.atts_changed;
  else ok=status==0&&!(disk_atts&FILE_ATTRIBUTE_REPARSE_POINT)&&f.atts_changed&&notifications==1&&f.inode_item.size==(c==3?0:17);
  ok=ok&&!locks; failed+=!ok;total++;
  printf("%s case%u: status=%08x atts_changed=%d reopened_atts=%x size=%llu truncates=%u notifications=%u\n",ok?"PASS":"FAIL",c,(unsigned)status,f.atts_changed,disk_atts,(unsigned long long)f.inode_item.size,truncates,notifications);
 }
 printf("%u/%u reparse scenarios passed\n",total-failed,total);
 return failed!=0;
}
