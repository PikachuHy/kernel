// kernel/init/shell/shell.cpp — Interactive serial shell
using uint64_t = unsigned long long; using uint32_t = unsigned int;
using int32_t = int; using uint8_t = unsigned char; using size_t = decltype(sizeof(0));
constexpr int SYS_DEBUG_PRINT=0,SYS_HANDLE_CLOSE=1,SYS_CHANNEL_WRITE=11,SYS_CHANNEL_READ=12;
constexpr int SYS_OPEN=50,SYS_PROCESS_EXIT=31,SYS_SERIAL_READ=54;
constexpr uint32_t O_RDONLY=1<<0;
struct FileMsg{enum Op:uint32_t{Open=0,Read=1,Write=2,Seek=3,Stat=4,Close=5,Readdir=6};Op op;uint32_t flags;uint64_t offset,length;};
struct FileResponse{int32_t result;uint64_t size;};
struct Stat{uint64_t size;uint32_t type;uint32_t padding;};
struct Dirent{char name[256];uint32_t type;uint64_t size;};
static uint64_t syscall6(uint64_t n,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5){
    uint64_t r;asm volatile("movq %1,%%rax\nmovq %2,%%rdi\nmovq %3,%%rsi\nmovq %4,%%rdx\nmovq %5,%%r10\nmovq %6,%%r8\nsyscall\nmovq %%rax,%0":"=r"(r):"r"(n),"r"(a1),"r"(a2),"r"(a3),"r"(a4),"r"(a5):"rax","rdi","rsi","rdx","r10","r8","rcx","r11","memory");return r;
}
static void p(const char*m){syscall6(SYS_DEBUG_PRINT,(uint64_t)m,0,0,0,0);}
static void pd(uint64_t n){if(!n){p("0");return;}char b[21];int i=20;b[i--]=0;while(n>0&&i>=0){b[i--]='0'+(n%10);n/=10;}p(&b[i+1]);}
static struct{const void*d;size_t sz;const uint32_t*hnd;size_t n;}sw;
static int cw(uint32_t h,const void*d,size_t n){sw.d=d;sw.sz=n;sw.hnd=nullptr;sw.n=0;return(int)syscall6(SYS_CHANNEL_WRITE,h,(uint64_t)&sw,0,0,0);}
static int cr(uint32_t h,void*b,size_t sz){return(int)syscall6(SYS_CHANNEL_READ,h,(uint64_t)b,sz,0,0);}
static void cc(uint32_t h){syscall6(SYS_HANDLE_CLOSE,h,0,0,0,0);}
static char getc(){return(char)(syscall6(SYS_SERIAL_READ,0,0,0,0,0)&0xFF);}
static void pc(char c){char s[2]={c,0};p(s);}
static void rl(char*buf,int mx){
    int i=0;
    while(i<mx-1){char c=getc();
        if(c=='\r'){pc('\n');buf[i++]='\n';break;}
        if(c=='\b'||c==127){if(i>0){i--;pc('\b');p(" ");pc('\b');}continue;}
        if(c<32)continue;
        pc(c);buf[i++]=c;
    }buf[i]=0;
}
static void fixpath(const char*in,char*out){int i=0;if(!in||!in[0]){out[0]='/';out[1]=0;return;}if(in[0]=='/'){while(in[i]){out[i]=in[i];i++;}out[i]=0;return;}out[0]='/';i=1;int j=0;while(in[j]){out[i]=in[j];i++;j++;}out[i]=0;}
static void cmd_ls(const char*raw){
    char path[256];fixpath(raw,path);
    uint32_t f=(uint32_t)syscall6(SYS_OPEN,(uint64_t)path,O_RDONLY,0,0,0);
    if(f==0||f==0xFFFFFFFF){p("  nf\n");return;}
    FileMsg m={FileMsg::Readdir,0,0,16};
    if(cw(f,&m,sizeof(m))!=0){cc(f);return;}
    uint8_t rb[sizeof(FileResponse)+16*sizeof(Dirent)];
    int rc=cr(f,rb,sizeof(rb));
    if(rc<(int)sizeof(FileResponse)){cc(f);return;}
    FileResponse*rp=(FileResponse*)rb;
    if(rp->result!=0){cc(f);return;}
    uint32_t cnt=rp->size/sizeof(Dirent);Dirent*dd=(Dirent*)(rb+sizeof(FileResponse));
    for(uint32_t i=0;i<cnt;i++){p("  ");p(dd[i].type?"[D]":"[F]");p(" ");p(dd[i].name);p(" (");pd(dd[i].size);p(")\n");}
    cc(f);
}
static void cmd_cat(const char*raw){
    char path[256];fixpath(raw,path);
    uint32_t f=(uint32_t)syscall6(SYS_OPEN,(uint64_t)path,O_RDONLY,0,0,0);
    if(f==0||f==0xFFFFFFFF){p("  nf\n");return;}
    FileMsg m={FileMsg::Read,0,0,64};
    if(cw(f,&m,sizeof(m))!=0){cc(f);return;}
    uint8_t rb[sizeof(FileResponse)+64];
    int rc=cr(f,rb,sizeof(rb));
    if(rc<(int)sizeof(FileResponse)){cc(f);return;}
    FileResponse*rp=(FileResponse*)rb;
    if(rp->result!=0){cc(f);return;}
    for(uint64_t i=0;i<rp->size&&i<64;i++){uint8_t b=rb[sizeof(FileResponse)+i];if(b>=32&&b<127){char c[2]={(char)b,0};p(c);}else p(".");}
    p("\n");cc(f);
}
static void cmd_stat(const char*raw){
    char path[256];fixpath(raw,path);
    uint32_t f=(uint32_t)syscall6(SYS_OPEN,(uint64_t)path,O_RDONLY,0,0,0);
    if(f==0||f==0xFFFFFFFF){p("  nf\n");return;}
    FileMsg m={FileMsg::Stat,0,0,0};
    if(cw(f,&m,sizeof(m))!=0){cc(f);return;}
    uint8_t rb[sizeof(FileResponse)+sizeof(Stat)];
    int rc=cr(f,rb,sizeof(rb));
    if(rc<(int)sizeof(FileResponse)){cc(f);return;}
    FileResponse*rp=(FileResponse*)rb;
    if(rp->result!=0){cc(f);return;}
    Stat*st=(Stat*)(rb+sizeof(FileResponse));
    p(st->type?"  Directory\n":"  File\n");p("  Size: ");pd(st->size);p("\n");cc(f);
}
extern "C" void _start(){
    p("\n=== Interactive Shell ===\n");
    p("Type 'help' for commands.\n");
    char line[256];
    while(1){
        p("shell> ");
        rl(line,256);p(line);
        {int j=0;while(line[j])j++;if(j>0&&line[j-1]=='\n')line[j-1]=0;}
        char*cmd=line,*arg=nullptr;
        while(*cmd==' ')cmd++;
        char*sp=cmd;while(*sp&&*sp!=' ')sp++;
        if(*sp==' '){*sp=0;arg=sp+1;while(*arg==' ')arg++;}
        if(cmd[0]==0||cmd[0]=='\n')continue;
        if(cmd[0]=='h'&&cmd[1]=='e'&&cmd[2]=='l'&&cmd[3]=='p'&&!cmd[4])p("Commands: help ls cat stat exit\n");
        else if(cmd[0]=='l'&&cmd[1]=='s'&&!cmd[2])cmd_ls(arg);
        else if(cmd[0]=='c'&&cmd[1]=='a'&&cmd[2]=='t'&&!cmd[3])cmd_cat(arg);
        else if(cmd[0]=='s'&&cmd[1]=='t'&&cmd[2]=='a'&&cmd[3]=='t'&&!cmd[4])cmd_stat(arg);
        else if(cmd[0]=='e'&&cmd[1]=='x'&&cmd[2]=='i'&&cmd[3]=='t'&&!cmd[4]){p("bye\n");break;}
        else p("  ? try help\n");
    }
    syscall6(SYS_PROCESS_EXIT,0,0,0,0,0);
    while(1)asm("hlt");
}
