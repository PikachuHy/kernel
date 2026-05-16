// kernel/init/init.cpp — syscall verification suite

using uint64_t = unsigned long long;
using uint32_t = unsigned int;
using int32_t  = int;
using uint8_t  = unsigned char;
using size_t = decltype(sizeof(0));

constexpr int SYS_DEBUG_PRINT    = 0;
constexpr int SYS_HANDLE_CLOSE   = 1;
constexpr int SYS_HANDLE_DUP     = 2;
constexpr int SYS_CHANNEL_CREATE = 10;
constexpr int SYS_CHANNEL_WRITE  = 11;
constexpr int SYS_CHANNEL_READ   = 12;
constexpr int SYS_PROCESS_EXIT   = 31;
constexpr int SYS_VMO_CREATE     = 40;
constexpr int SYS_OPEN           = 50;

constexpr uint64_t INV = static_cast<uint64_t>(-1);

struct FileMsg {
    enum Op : uint32_t { Open=0, Read=1, Write=2, Seek=3, Stat=4, Close=5, Readdir=6 };
    Op op; uint32_t flags; uint64_t offset; uint64_t length;
};
struct FileResponse { int32_t result; uint64_t size; };

// ── syscall6 ──
static uint64_t s6(uint64_t n, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t r;
    asm volatile("movq %1,%%rax; movq %2,%%rdi; movq %3,%%rsi; movq %4,%%rdx; movq %5,%%r10; movq %6,%%r8; syscall; movq %%rax,%0"
        : "=r"(r) : "r"(n),"r"(a1),"r"(a2),"r"(a3),"r"(a4),"r"(a5)
        : "rax","rdi","rsi","rdx","r10","r8","rcx","r11","memory");
    return r;
}
static void pr(const char* m) { s6(SYS_DEBUG_PRINT,(uint64_t)m,0,0,0,0); }
static void ph(uint64_t n) {
    char b[20]="0x0000000000000000\n";
    for(int i=17;i>1;i--){uint8_t d=n&0xF;b[i]=d<10?'0'+d:'A'+d-10;n>>=4;}
    pr(b);
}
static int cw(uint32_t h,const void* d,size_t s){
    struct WA{const void* d;size_t s;const uint32_t* h;size_t n;};
    WA a={d,s,0,0};return(int)s6(SYS_CHANNEL_WRITE,h,(uint64_t)&a,0,0,0);
}
static int cr(uint32_t h,void* b,size_t s){
    return(int)s6(SYS_CHANNEL_READ,h,(uint64_t)b,s,0,0);
}
static void cl(uint32_t h){s6(SYS_HANDLE_CLOSE,h,0,0,0,0);}

// ── test harness ──
static int pass=0,fail=0;
#define T_OK(m) do{pr("  [PASS] " m "\n");pass++;}while(0)
#define T_FAIL(m,r) do{pr("  [FAIL] " m ": " r "\n");fail++;}while(0)

extern "C" void _start(){
    pr("\n=== init: syscall test ===\n\n");

    // 1. debug_print
    pr("1. debug_print: "); s6(SYS_DEBUG_PRINT,(uint64_t)"[ok]\n",0,0,0,0); T_OK("ok");

    // 2. channel_create
    pr("2. channel_create: ");
    uint64_t p=s6(SYS_CHANNEL_CREATE,0,0,0,0,0);
    uint32_t a=(uint32_t)(p>>32),b=(uint32_t)(p&0xFFFFFFFF);
    if(p==INV||a==0||b==0||a==b) T_FAIL("channel_create","bad");
    else{
        T_OK("channel_create");

        // 3. channel IPC ping-pong
        pr("3. channel IPC: ");
        if(cw(a,"ping",4)!=0) T_FAIL("write","fail");
        else{char rb[16]={};int rr=cr(b,rb,sizeof(rb));
            if(rr!=4) T_FAIL("read","bad len");
            else if(rb[0]!='p'||rb[1]!='i') T_FAIL("data","mismatch");
            else T_OK("ping-pong");
        }
        cl(a);cl(b);
    }

    // 4. handle_dup
    pr("4. handle_dup: ");
    p=s6(SYS_CHANNEL_CREATE,0,0,0,0,0);
    if(p==INV) T_FAIL("handle_dup","create fail");
    else{
        a=(uint32_t)(p>>32);b=(uint32_t)(p&0xFFFFFFFF);
        uint64_t d=s6(SYS_HANDLE_DUP,a,1,0,0,0);
        if(d==INV) T_FAIL("handle_dup","dup fail");
        else{T_OK("handle_dup");cl((uint32_t)d);}
        cl(a);cl(b);
    }

    // 5. VFS open /dev/console
    pr("5. VFS /dev/console: ");
    uint64_t ch=s6(SYS_OPEN,(uint64_t)"/dev/console",2,0,0,0);
    if(ch==INV||ch==0) T_FAIL("open /dev/console","bad handle");
    else{
        T_OK("open");
        // write greeting via FileMsg
        uint8_t wb[40]; FileMsg* w=(FileMsg*)wb;
        w->op=FileMsg::Write;w->flags=0;w->offset=0;w->length=16;
        const char* g="Hello from init!\n";
        for(int i=0;i<16;i++)wb[24+i]=(uint8_t)g[i];
        cw(ch,wb,40); FileResponse r; cr(ch,&r,sizeof(r)); cl(ch);
    }

    // 6. VFS tmpfs create+read
    pr("6. VFS /test.txt: ");
    uint64_t fh=s6(SYS_OPEN,(uint64_t)"/test.txt",0xC,0,0,0);
    if(fh==INV||fh==0) T_FAIL("tmpfs","bad handle");
    else{
        T_OK("open+create");
        // write
        uint8_t wb[40]; FileMsg* w=(FileMsg*)wb;
        w->op=FileMsg::Write;w->flags=0;w->offset=0;w->length=13;
        const char* d="Hello, tmpfs!";
        for(int i=0;i<13;i++)wb[24+i]=(uint8_t)d[i];
        cw(fh,wb,37); FileResponse wr; cr(fh,&wr,sizeof(wr));
        // read back
        w->op=FileMsg::Read;w->flags=0;w->offset=0;w->length=64;
        cw(fh,w,sizeof(FileMsg));
        uint8_t rb[80]; cr(fh,rb,sizeof(rb));
        FileResponse* rr=(FileResponse*)rb;
        pr("    read size="); ph(rr->size); pr(" data='");
        for(size_t i=0;i<rr->size&&i<13;i++){char c[2]={(char)rb[16+i],0};pr(c);}
        pr("'\n"); cl(fh);
    }

    // 7. VMO create (map deferred — requires valid user VA range)
    pr("7. VMO create: ");
    uint64_t vh=s6(SYS_VMO_CREATE,4096,0,0,0,0);
    if(vh==INV||vh==0) T_FAIL("vmo_create","fail");
    else{T_OK("vmo_create");cl(vh);}

    // summary
    pr("\n=== "); ph(pass); pr(" passed, "); ph(fail); pr(" failed ===\n");
    pr("=== init: done ===\n");
    s6(SYS_PROCESS_EXIT,0,0,0,0,0);
    while(1) asm volatile("hlt");
}
