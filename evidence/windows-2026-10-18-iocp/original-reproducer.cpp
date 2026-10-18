#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <efsw/WatcherWin32.hpp>
static void* watched=nullptr;
static ULONG_PTR atDelete=0;
void operator delete(void* p) noexcept {
 if(p==watched && p) { atDelete=((OVERLAPPED*)p)->Internal; return; }
 std::free(p);
}
void operator delete(void* p,size_t) noexcept { ::operator delete(p); }
int main() {
 wchar_t path[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH,path);
 HANDLE port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1);
 for(int i=0;i<10000;++i) {
  auto watch=efsw::CreateWatch(path,false,63*1024,FILE_NOTIFY_CHANGE_FILE_NAME,port);
  if(!watch){printf("create failed %lu\n",GetLastError());return 2;}
  watched=watch; atDelete=0;
  efsw::DestroyWatch(watch);
  auto first=atDelete; auto after=((volatile OVERLAPPED*)watch)->Internal;

  DWORD bytes;ULONG_PTR key;OVERLAPPED* completed=nullptr;
  BOOL result=GetQueuedCompletionStatus(port,&bytes,&key,&completed,5000);
  if(completed!=&watch->Overlapped){printf("completion missing %lu\n",GetLastError());return 3;}
  after=((volatile OVERLAPPED*)watch)->Internal;
  if(first!=after) { printf("PROVEN iteration=%d address=%p bytes=%zu status_at_delete=0x%llx status_after_delete=0x%llx completion_ok=%d\n",i,watch,sizeof(*watch),(unsigned long long)first,(unsigned long long)after,result); std::free(watch);CloseHandle(port);return 1; }
  std::free(watch); watched=nullptr;
 }
 printf("No delayed write observed in 10000 iterations\n");CloseHandle(port);return 0;
}
