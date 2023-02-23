//描述：守护进程初始化
//执行失败：返回-1,  子进程：返回0，父进程：返回1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>     //errno
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>    //信号相关头文件 
#include <sys/wait.h>  //waitpid
#define DEF_PROCESS_MASTER     0  //master进程，管理进程
#define DEF_PROCESS_WORKER     1  //worker进程，工作进程
int g_def_process;  
int g_def_get_child_exit=0;
pid_t g_def_parent_pid = 0;         
pid_t g_def_self_pid = 0;                

int def_daemon();
static void def_signal_handler(int signo, siginfo_t *siginfo, void *ucontext); //声明一个信号处理函数
static void g_def_process_get_status(void);                                      //获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程
int def_init_signals();
static int def_spawn_process(int inum);


//一个信号有关的结构 def_signal_t
typedef struct 
{
    int           signo;       //信号对应的数字编号
    const  char   *signame;    //信号对应的名字 ，比如SIGHUP 
    //信号处理函数,这个函数由我们自己来提供，但是它的参数和返回值是系统定义的
    void  (*handler)(int signo, siginfo_t *siginfo, void *ucontext); //函数指针, siginfo_t:系统定义的结构
} def_signal_t;

//定义处理的各种信号
def_signal_t  signals[] = {
    // signo      signame             handler
    { SIGHUP,    "SIGHUP",           def_signal_handler },        //终端断开信号，对于守护进程常用于reload重载配置文件通知--标识1
    { SIGINT,    "SIGINT",           def_signal_handler },        //标识2   
	{ SIGTERM,   "SIGTERM",          def_signal_handler },        //标识15
    { SIGCHLD,   "SIGCHLD",          def_signal_handler },        //子进程退出时，父进程会收到这个信号--标识17
    { SIGQUIT,   "SIGQUIT",          def_signal_handler },        //标识3
    { SIGIO,     "SIGIO",            def_signal_handler },        //指示一个异步I/O事件【通用异步I/O信号】
    { SIGSYS,    "SIGSYS, SIG_IGN",  NULL               },        //我们想忽略这个信号，SIGSYS表示收到了一个无效系统调用，如果我们不忽略，进程会被操作系统杀死，--标识31
                                                                  //所以我们把handler设置为NULL，代表 我要求忽略这个信号，请求操作系统不要执行缺省的该信号处理动作（杀掉我）
    //...
    { 0,         NULL,               NULL               }         //信号对应的数字至少是1，所以可以用0作为一个特殊结束标记
};

//printf函数应该由写文件的log函数代替
int def_daemon()
{
    g_def_self_pid=getpid();
    //(1)创建守护进程的第一步，fork()一个子进程出来
    switch (fork())  //fork()出来这个子进程才会成为咱们这里讲解的master进程；
    {
    case -1:
        //创建子进程失败
        printf("fork()失败!");
        return -1;
    case 0:
        //子进程，走到这里直接break;
        break;
    default:
        //父进程以往 直接退出exit(0);现在希望回到主流程去释放一些资源
        return 1;  //父进程直接返回1；
    } 
    //只有fork()出来的子进程才能走到这个流程
    g_def_parent_pid = g_def_self_pid;     //g_def_self_pid是原来父进程的id，因为这里是子进程，所以子进程的设置为原来父进程的pid
    g_def_self_pid = getpid();       //当前子进程的id要重新取得
    //(2)脱离终端，终端关闭，将跟此子进程无关
    if (setsid() == -1)  
    {
        printf("setsid()失败!");
        return -1;
    }
    if(chdir("/")==-1)
    {
        printf("setsid()失败!");
        return -1;
    }
    //(3)设置为0，不要让它来限制文件权限，以免引起混乱
    umask(0); 
    //(4)打开黑洞设备，以读写方式打开
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) 
    {
        printf("open(\"/dev/null\")失败!");        
        return -1;
    }
    //(5)打开黑洞设备，以读写方式打开
    if (dup2(fd, STDIN_FILENO) == -1) //重定向/dev/null成为标准输入；
    {
        printf("dup2(STDIN)失败!");        
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) //重定向/dev/null成为标准输出；
    {
        printf("dup2(STDOUT)失败!");
        return -1;
    }
    if (fd > STDERR_FILENO)  //fd应该是3，这个应该成立
     {
        if (close(fd) == -1)  //释放资源这样这个文件描述符就可以被复用；【文件描述符】会被一直占着；
        {
            printf( "close(fd)失败!");
            return -1;
        }
    }
    return 0; //子进程返回0
}

//初始化信号的函数，用于注册信号处理程序
//返回值：0成功  ，-1失败
int def_init_signals()
{
    def_signal_t      *sig;  //指向自定义结构数组的指针 
    struct sigaction   sa;   //sigaction：系统定义的跟信号有关的一个结构，我们后续调用系统的sigaction()函数时要用到这个同名的结构
    for (sig = signals; sig->signo != 0; sig++)  //将signo ==0作为一个标记，因为信号的编号都不为0；
    {        
        memset(&sa,0,sizeof(struct sigaction));
        if (sig->handler)  //如果信号处理函数不为空，这当然表示我要定义自己的信号处理函数
        {
            sa.sa_sigaction = sig->handler;  //sa_sigaction：指定信号处理程序(函数)，注意sa_sigaction也是函数指针，是这个系统定义的结构sigaction中的一个成员（函数指针成员）；
            sa.sa_flags = SA_SIGINFO;  //sa_flags：int型，指定信号的一些选项，设置了该标记(SA_SIGINFO)，就表示信号附带的参数可以被传递到信号处理函数中,sa.sa_sigaction指定的信号处理程序(函数)生效，你就把sa_flags设定为SA_SIGINFO
        }
        else
        {
            sa.sa_handler = SIG_IGN; //sa_handler:这个标记SIG_IGN给到sa_handler成员，表示忽略信号的处理程序，否则操作系统的缺省信号处理程序很可能把这个进程杀掉；
                                      //其实sa_handler和sa_sigaction都是一个函数指针用来表示信号处理程序。只不过这两个函数指针他们参数不一样， sa_sigaction带的参数多，信息量大，
                                       //而sa_handler带的参数少，信息量少；如果你想用sa_sigaction，那么你就需要把sa_flags设置为SA_SIGINFO；                                       
        } 
        sigemptyset(&sa.sa_mask);   //比如咱们处理某个信号比如SIGUSR1信号时不希望收到SIGUSR2信号，那咱们就可以用诸如sigaddset(&sa.sa_mask,SIGUSR2);这样的语句针对信号为SIGUSR1时做处理
                                    //这里.sa_mask是个信号集（描述信号的集合），用于表示要阻塞的信号，sigemptyset()这个函数咱们在第三章第五节讲过：把信号集中的所有信号清0，就是不准备阻塞任何信号；
                                    
        //设置信号处理动作(信号处理函数)，让这个信号来了后调用我的处理程序，有个老的同类函数叫signal，不过signal这个函数被认为是不可靠信号语义，不建议使用，用sigaction
        if (sigaction(sig->signo, &sa, NULL) == -1) //参数1：要操作的信号
                                                     //参数2：主要就是那个信号处理函数以及执行信号处理函数时候要屏蔽的信号等等内容
                                                      //参数3：返回以往的对信号的处理方式【跟sigprocmask()函数边的第三个参数是的】，跟参数2同一个类型，我们这里不需要这个东西，所以直接设置为NULL；
        {   
            printf("sigaction(%s) failed",sig->signame); //显示到日志文件中去的 
            return -1; //有失败就直接返回
        }	
        else
        {            
            //printf("sigaction(%s) succed!",sig->signame);     //成功不用写日志 
            //printf("sigaction(%s) succed!",sig->signame); //直接往屏幕上打印看看 ，不需要时可以去掉
        }
    } 
    return 0;    
}

//获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程
static void g_def_process_get_status(void)
{
    pid_t            pid;
    int              status;
    int              err;
    int              one=0; //应该是标记信号正常处理过一次

    //当你杀死一个子进程时，父进程会收到这个SIGCHLD信号。
    for ( ;; ) 
    {
        //waitpid获取子进程的终止状态，这样，子进程就不会成为僵尸进程了；
        //第一次waitpid返回一个> 0值，表示成功
        //第二次再循环回来，再次调用waitpid会返回一个0，表示子进程还没结束，然后这里有return来退出；
        pid = waitpid(-1, &status, WNOHANG); //第一个参数为-1，表示等待任何子进程，
                                              //第二个参数：保存子进程的状态信息
                                               //第三个参数：提供额外选项，WNOHANG表示不要阻塞，让这个waitpid()立即返回        

        if(pid == 0) //子进程没结束，会立即返回这个数字，但这里应该不是这个数字【因为一般是子进程退出时会执行到这个函数】
        {
            return;
        } //end if(pid == 0)
        //-------------------------------
        if(pid == -1)//这表示这个waitpid调用有错误，有错误也理解返回出去
        {
            //主要目的是打印一些日志。考虑到这些代码也许比较成熟，
            err = errno;
            if(err == EINTR)           //调用被某个信号中断
            {
                continue;
            }

            if(err == ECHILD  && one)  //没有子进程
            {
                return;
            }

            if (err == ECHILD)         //没有子进程
            {
                printf("waitpid() failed!");
                return;
            }
            printf("waitpid() failed!");
            return;
        } 
        //走到这里，表示  成功【返回进程id】 ，打印一些日志来记录子进程的退出
        one = 1;  //标记waitpid()返回了正常的返回值
        if(WTERMSIG(status))  //获取使子进程终止的信号编号
        {
           printf("pid = %x exited on signal %d!",pid,WTERMSIG(status)); //获取使子进程终止的信号编号
        }
        else
        {
            printf("pid = %x exited with code %d!",pid,WEXITSTATUS(status)); //WEXITSTATUS()获取子进程传递给exit或者_exit参数的低八位
        }
    } //end for
    return;
}
//信号处理函数
//siginfo：这个系统定义的结构中包含了信号产生原因的有关信息
static void def_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{    
    //printf("来信号了\n");    
    def_signal_t    *sig;    //自定义结构
    char            *action; //一个字符串，用于记录一个动作字符串以往日志文件中写
    
    for (sig = signals; sig->signo != 0; sig++) //遍历信号数组    
    {         
        //找到对应信号，即可处理
        if (sig->signo == signo) 
        { 
            break;
        }
    } //end for

    action = (char *)"";  //目前还没有什么动作；

    if(g_def_process == DEF_PROCESS_MASTER)      //master进程，管理进程，处理的信号一般会比较多 
    {
        //master进程的往这里走
        switch (signo)
        {
        case SIGCHLD:  //一般子进程退出会收到该信号
            g_def_get_child_exit = 1;  //标记子进程状态变化，日后master主进程的for(;;)循环中用到这个变量【比如重新产生一个子进程】
            break;
        //.....其他信号
        default:
            break;
        } 
    }
    else if(g_def_process == DEF_PROCESS_WORKER) //worker进程，具体干活的进程，处理的信号相对比较少
    {
        //worker进程的往这里走
        //......以后再增加
        //....
    }
    else
    {
        //非master非worker进程，先啥也不干
        //do nothing
    } //end if(g_def_process == DEF_PROCESS_MASTER)

    //这里记录一些日志信息
    //siginfo这个
    if(siginfo && siginfo->si_pid)  //si_pid = sending process ID【发送该信号的进程id】
    {
        printf( "signal %d (%s) received from %x%s", signo, sig->signame, siginfo->si_pid, action); 
    }
    else
    {
        printf( "signal %d (%s) received %s",signo, sig->signame, action);//没有发送该信号的进程id，所以不显示发送该信号的进程id
    }
    //子进程状态有变化，通常是意外退出【既然官方是在这里处理，我们也学习官方在这里处理】
    if (signo == SIGCHLD) 
    {
        g_def_process_get_status(); //获取子进程的结束状态
    } //end if
    return;
}

//描述：产生一个子进程
//inum：进程编号【0开始】
//pprocname：子进程名字"worker process"
static int def_spawn_process(int inum)
{
    pid_t  pid;

    pid = fork(); //fork()系统调用产生子进程
    switch (pid)  //pid判断父子进程，分支处理
    {  
    case -1: //产生子进程失败
        printf("def_spawn_process()fork()产生子进程num=%d 失败!",inum);
        return -1;

    case 0:  //子进程分支
        g_def_parent_pid = g_def_self_pid;              //因为是子进程了，所有原来的pid变成了父pid
        g_def_self_pid = getpid();                //重新获取pid,即本子进程的pid
        //所有worker子进程，在这个函数里不断循环着不出来，也就是说，子进程流程不往下边走;
        while(1)
        {
            //work 代码
            sleep(1);
        }
        break;

     default: //这个应该是父进程分支，直接break;，流程往switch之后走            
        break;
    }//end switch

    //父进程分支会走到这里，子进程流程不往下边走-------------------------
    //若有需要，以后再扩展增加其他代码......
    return pid;
}
//描述：创建worker子进程
void master_process_cycle()
{    
    sigset_t set;        //信号集
    sigemptyset(&set);   //清空信号集
    //下列这些信号在执行本函数期间不希望收到（保护不希望由信号中断的代码临界区）
    //建议fork()子进程时防止信号的干扰；
    sigaddset(&set, SIGCHLD);     //子进程状态改变
    sigaddset(&set, SIGALRM);     //定时器超时
    sigaddset(&set, SIGIO);       //异步I/O
    sigaddset(&set, SIGINT);      //终端中断符
    sigaddset(&set, SIGHUP);      //连接断开
    sigaddset(&set, SIGUSR1);     //用户定义信号
    sigaddset(&set, SIGUSR2);     //用户定义信号
    sigaddset(&set, SIGWINCH);    //终端窗口大小改变
    sigaddset(&set, SIGTERM);     //终止
    sigaddset(&set, SIGQUIT);     //终端退出符
    //.........需要往其中添加其他要屏蔽的信号
    //设置，此时无法接受的信号；阻塞期间，你发过来的上述信号，多个会被合并为一个，暂存着，等你放开信号屏蔽后才能收到这些信号。。。
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) //第一个参数用了SIG_BLOCK表明设置 进程 新的信号屏蔽字 为 “当前信号屏蔽字 和 第二个参数指向的信号集的并集
    {        
        printf("ngx_master_process_cycle()中sigprocmask()失败!");
    }
    //即便sigprocmask失败，程序流程 也继续往下走
    for (int i = 0; i < 5; i++)  //master进程在走这个循环，来创建5个子进程
    {
        def_spawn_process(i);
    } 
    //创建子进程后，父进程的执行流程会返回到这里，子进程不会走进来    
    sigemptyset(&set); //信号屏蔽字为空，表示不屏蔽任何信号
    for ( ;; ) 
    {

        //usleep(100000);
        //sigsuspend(const sigset_t *mask))用于在接收到某个信号之前, 临时用mask替换进程的信号掩码, 并暂停进程执行，直到收到信号为止。
        //sigsuspend 返回后将恢复调用之前的信号掩码。信号处理函数完成后，进程将继续执行。该系统调用始终返回-1，并将errno设置为EINTR。
        //sigsuspend是一个原子操作，包含4个步骤：
        //a)根据给定的参数设置新的mask 并 阻塞当前进程【因为是个空集，所以不阻塞任何信号】
        //b)此时，一旦收到信号，便恢复原先的信号屏蔽【我们原来调用sigprocmask()的mask在上边设置的，阻塞了多达10个信号，从而保证我下边的执行流程不会再次被其他信号截断】
        //c)调用该信号对应的信号处理函数
        //d)信号处理函数返回后，sigsuspend返回，使程序流程继续往下走
        sigsuspend(&set); //阻塞在这里，等待一个信号，此时进程是挂起的，不占用cpu时间，只有收到信号才会被唤醒（返回）；
                         //此时master进程完全靠信号驱动干活    
        
        //printf("master进程休息1秒\n");      
        sleep(1); //休息1秒        

    }
    return;
}
int main()
{
    int exitcode=0;
    int ret=0;
    if( def_init_signals() != 0) //信号初始化
    {
        exitcode = 1;
        goto lblexit;
    } 
    ret= def_daemon();
    if(ret == -1) //fork()失败
    {
        exitcode = 1;//标记失败
        goto lblexit;
    }
    if(ret == 1)
    {    
        //父进程退出
        //这里可以释放资源
        exitcode = 0;
        return exitcode;  //整个进程直接在这里退出
    }
    master_process_cycle();
    //走到这里，成功创建了守护进程并且这里已经是fork()出来的子进程，由父进程1接管
    //(7)开始守护进程的主工作流程 

    for(;;)    
    {
       sleep(1); //休息1秒        
       printf("休息1秒\n");        
    }
lblexit:
    //(5)该释放的资源要释放掉
    printf("程序退出!");
    return exitcode;
}

