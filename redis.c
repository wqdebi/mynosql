#define REDIS_VERSION "1.000"

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __USE_POSIX199309
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>

#include "redis.h"
#include "ae.h"     /* Event driven programming library */
#include "sds.h"    /* Dynamic safe strings */
#include "anet.h"   /* Networking the easy way */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "lzf.h"    /* LZF compression library */
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */

/* Error codes */
#define REDIS_OK                0
#define REDIS_ERR               -1

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_IOBUF_LEN         1024
#define REDIS_LOADBUF_LEN       1024
#define REDIS_STATIC_ARGS       4
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_OBJFREELIST_MAX   1000000 /* Max number of objects to cache */
#define REDIS_MAX_SYNC_TIME     60      /* Slave can't take more to sync */
#define REDIS_EXPIRELOOKUPS_PER_CRON    100 /* try to expire 100 keys/second */
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE  (1024*1024*256) /* max bytes in inline command */

/* Hash table parameters */
#define REDIS_HT_MINFILL        10      /* Minimal hash table fill 10% */

/* Command flags */
#define REDIS_CMD_BULK          1       /* Bulk write command */
#define REDIS_CMD_INLINE        2       /* Inline command */
/* REDIS_CMD_DENYOOM reserves a longer comment: all the commands marked with
   this flags will return an error when the 'maxmemory' option is set in the
   config file and the server is using more than maxmemory bytes of memory.
   In short this commands are denied on low memory conditions. */
#define REDIS_CMD_DENYOOM       4

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_HASH 3

/* Object types only used for dumping to disk */
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lenghts up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* Client flags */
#define REDIS_CLOSE 1       /* This client connection should be closed ASAP */
#define REDIS_SLAVE 2       /* This client is a slave server */
#define REDIS_MASTER 4      /* This client is a master server */
#define REDIS_MONITOR 8      /* This client is a slave monitor, see MONITOR */

/* Slave replication state - slave side */
#define REDIS_REPL_NONE 0   /* No active replication */
#define REDIS_REPL_CONNECT 1    /* Must connect to master */
#define REDIS_REPL_CONNECTED 2  /* Connected to master */

/* Slave replication state - from the point of view of master
 * Note that in SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE state instead the server is waiting
 * to start the next background saving in order to send updates to it. */
#define REDIS_REPL_WAIT_BGSAVE_START 3 /* master waits bgsave to start feeding it */
#define REDIS_REPL_WAIT_BGSAVE_END 4 /* master waits bgsave to start bulk DB transmission */
#define REDIS_REPL_SEND_BULK 5 /* master is sending the bulk DB */
#define REDIS_REPL_ONLINE 6 /* bulk DB already transmitted, receive updates */

/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_DEL 1
#define REDIS_SORT_INCR 2
#define REDIS_SORT_DECR 3
#define REDIS_SORT_ASC 4
#define REDIS_SORT_DESC 5
#define REDIS_SORTKEY_MAX 1024

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_NOTICE 1
#define REDIS_WARNING 2

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)


/*================================= Data types ============================== */

/* A redis object, that is a type able to hold a string / list / set */
//redis对象，这是一种能够容纳字符串/列表/集合的类型
typedef struct redisObject {
    void *ptr;
    int type;
    int refcount;
} robj;

//结构体是用来表示一个数据库实例的。Redis支持多个数据库。
typedef struct redisDb {
    dict *dict;//存储数据库中的所有键值对。
    dict *expires;//存储设置了过期时间的键
    int id;
} redisDb;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    int fd;
    //指向当前选中的 redisDb 数据库的指针。
    //Redis 支持多数据库，但客户端在同一时间只能操作一个数据库。这个指针就是用来指向当前客户端正在操作的那个数据库。
    redisDb *db;
    //当前客户端选中的数据库 ID。虽然 db 指针已经指向了当前数据库，但有时候直接通过 ID 来引用数据库也是方便的。
    int dictid;
    //用于存储客户端发送的查询命令的缓冲区。
    sds querybuf;
    //指向命令参数数组的指针。当 Redis 解析了客户端发送的命令后，它会将命令的参数存储在这个数组中。
    robj **argv;
    //命令参数的数量。这个值等于 argv 数组的长度。
    int argc;
    //如果客户端正在执行批量读取操作（如 GET 命令读取大量数据），
    //这个字段会存储需要读取的数据长度。如果不是批量读取模式，则这个值为 -1
    int bulklen;            /* bulk read len. -1 if not in bulk read mode */
    //指向回复队列的指针。Redis 会将命令的回复添加到这个队列中，然后通过套接字发送给客户端。
    list *reply;
    //已经发送给客户端的回复字节数。这个值用于跟踪回复的发送进度，特别是在处理大回复时。
    int sentlen;
    //上次与客户端交互的时间戳。这个值用于实现客户端超时机制，如果客户端在一定时间内没有与服务器交互，服务器可能会关闭这个连接。
    time_t lastinteraction; /* time of the last interaction, used for timeout */
    //标志位字段，用于存储关于客户端的各种状态信息。例如，REDIS_CLOSE 表示这个连接应该被关闭，
    //REDIS_SLAVE 表示这个客户端是一个从服务器（slave），REDIS_MONITOR 表示这个客户端正在监视（monitor）服务器上的操作。
    int flags;              /* REDIS_CLOSE | REDIS_SLAVE | REDIS_MONITOR */
    //如果这个客户端是一个从服务器，这个字段存储了它选择的数据库 ID。这个值在主从复制过程中用于同步数据。
    int slaveseldb;         /* slave selected db, if this client is a slave */
    //认证标志位。如果 Redis 设置了密码（requirepass），则客户端在连接后必须进行认证。
    int authenticated;      /* when requirepass is non-NULL */
    //如果这个客户端是一个从服务器，这个字段将存储其复制状态
    int replstate;          /* replication state if this is a slave */
    //复制数据库文件描述符。
    int repldbfd;           /* replication DB file descriptor */
    //复制数据库文件的当前偏移量。在从服务器读取主服务器发送的数据时，这个值会不断更新，以指示已经读取了多少数据。
    long repldboff;          /* replication DB file offset */
    //复制数据库文件的大小。这个值用于在从服务器启动复制过程时，确定需要下载多少数据。
    off_t repldbsize;       /* replication DB file size */
} redisClient;

struct saveparam {
    time_t seconds;
    int changes;
};

/* Global server state structure */
struct redisServer {
    //服务器监听的端口号。
    int port;
    //服务器套接字的文件描述符，用于监听和接受客户端连接。
    int fd;
    //指向一个 redisDb 数组的指针，Redis 支持多数据库，这个数组包含了所有的数据库实例。
    redisDb *db;
    //用于对象共享池，Redis 可以配置为共享某些类型的对象以节省内存。这个字典和大小分别用于存储共享对象和跟踪共享池的大小。
    dict *sharingpool;
    //对象共享池大小
    unsigned int sharingpoolsize;
    //自上次保存数据库以来，数据库被修改的次数
    long long dirty;            /* changes to DB from the last save */
    //一个列表，包含了所有连接到服务器的客户端的 redisClient 结构体指针。
    list *clients;
    //分别包含从服务器（slave）和监视器（monitor）的列表。
    list *slaves, *monitors;
    //用于存储网络错误的缓冲区。
    char neterr[ANET_ERR_LEN];
    //指向 Redis 事件循环的指针，
    aeEventLoop *el;
    //cron 函数（Redis 的定时任务处理函数）运行的次数。
    int cronloops;              /* number of times the cron function run */
    //一个列表，用于存储已经释放但可能再次使用的对象，以减少内存分配的开销。
    list *objfreelist;          /* A list of freed objects to avoid malloc() */
    //后一次成功保存数据库的时间戳。
    time_t lastsave;            /* Unix time of last save succeeede */
    //Redis 服务器当前使用的内存量
    size_t usedmemory;             /* Used memory in megabytes */
    /* Fields used only for stats */
    time_t stat_starttime;         /* server start time 服务器启动的时间戳。*/
    long long stat_numcommands;    /* number of processed commands 服务器处理过的命令总数*/
    long long stat_numconnections; /* number of connections received 服务器接收到的连接总数*/
    /* Configuration */
    int verbosity;//日志级别
    int glueoutputbuf;//是否将输出合并 使用粘包
    int maxidletime;//最大空闲时间
    int dbnum;//数据库数量
    int daemonize;//是否作为守护进程运行
    char *pidfile;//PID 文件路径
    int bgsaveinprogress;//是否正在进行后台保存
    pid_t bgsavechildpid;//后台保存子进程的 PID
    struct saveparam *saveparams;//保存参数
    int saveparamslen;//参数长度
    char *logfile;//日志文件路径
    char *bindaddr;//绑定地址
    char *dbfilename;//数据库文件名
    char *requirepass;//密码
    int shareobjects;//是否共享对象
    /* Replication related */
    int isslave;//指示当前服务器是否是一个从服务器。
    char *masterhost;//主服务器的地址
    int masterport;//主服务器的端口号
    redisClient *master;    /* client that is master for this slave 指向连接到主服务器的客户端的指针 */
    int replstate;//复制状态。
    unsigned int maxclients;//服务器允许的最大客户端连接数。
    unsigned int maxmemory;//服务器允许使用的最大内存量
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
};

typedef void redisCommandProc(redisClient *c);
struct redisCommand {
    char *name;//命令的名称，是一个以 null 结尾的字符串。例如，对于 SET 命令，这个成员的值将是 "SET"。
    redisCommandProc *proc;//该函数指针指向处理该命令的函数。
    int arity;//命令的参数个数。Redis 命令可以接受不同数量的参数，arity 成员指定了该命令必须接收的参数数量
    //一个标志位字段，用于存储与该命令相关的各种标志。这些标志可以指示命令的性质，比如命令是否可以在脚本中执行、
    //是否可以在订阅/发布通道上执行、是否需要加载数据库等。Redis 使用位掩码的方式来设置和检查这些标志。
    int flags;
};
//定义了一种方式来存储和引用函数或代码段的名称及其对应的内存地址（或指针）
struct redisFunctionSym {
    //这是一个指向字符数组的指针，用于存储函数或代码段的名称。这个名称通常是一个以 null 结尾的字符串，用于唯一标识函数或代码段。
    char *name;
    unsigned long pointer;//用于存储函数或代码段的内存地址
};

typedef struct _redisSortObject {
    robj *obj;//这是一种能够容纳字符串/列表/集合的类型
    union {
        double score;//分数
        robj *cmpobj;//于在执行基于对象比较的排序时，存储与当前对象进行比较的另一个对象。
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;
//920行初始化
struct sharedObjectsStruct {
    //crlf 指向一个包含换行符（CRLF，即 \r\n）的字符串对象
    //ok 和 err 分别指向表示成功和错误响应的字符串对象。
    //emptybulk 和 nullbulk 用于表示空的批量回复（bulk reply）和空值的批量回复。
    //czero 和 cone 分别指向表示数字 0 和 1 的字符串对象。
    //pong 用于处理 PING 命令的响应。
    //select0 到 select9 用于处理数据库选择命令的响应
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *plus,
    *select0, *select1, *select2, *select3, *select4,
    *select5, *select6, *select7, *select8, *select9;
} shared;

/*================================ Prototypes =============================== */
//释放robj中的String对象
static void freeStringObject(robj *o);
//释放robj中的String对象
static void freeListObject(robj *o);
//释放哈希表
static void freeSetObject(robj *o);
//引用计数-1,根据类型type释放ptr，将o添加到一个名为server.objfreelist的列表
//如果超过REDIS_OBJFREELIST_MAX或者添加失败则会释放o
static void decrRefCount(void *o);
//创建一个robj*对象
static robj *createObject(int type, void *ptr);
//释放客户端结构体
static void freeClient(redisClient *c);
static int rdbLoad(char *filename);
static void addReply(redisClient *c, robj *obj);
static void addReplySds(redisClient *c, sds s);
static void incrRefCount(robj *o);
static int rdbSaveBackground(char *filename);
static robj *createStringObject(char *ptr, size_t len);
static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc);
static int syncWithMaster(void);
//这段代码实现了一个简单的对象共享池机制，用于在Redis中减少内存使用，特别是针对字符串类型的对象。
//它通过维护一个字典（server.sharingpool）来跟踪哪些字符串对象已经被共享，并允许后续请求重用这些对象而不是创建新的对象
static robj *tryObjectSharing(robj *o);
static int removeExpire(redisDb *db, robj *key);
static int expireIfNeeded(redisDb *db, robj *key);
static int deleteIfVolatile(redisDb *db, robj *key);
static int deleteKey(redisDb *db, robj *key);
static time_t getExpire(redisDb *db, robj *key);
static int setExpire(redisDb *db, robj *key, time_t when);
static void updateSlavesWaitingBgsave(int bgsaveerr);
static void freeMemoryIfNeeded(void);
static int processCommand(redisClient *c);
static void setupSigSegvAction(void);
static void rdbRemoveTempFile(pid_t childpid);

static void authCommand(redisClient *c);
static void pingCommand(redisClient *c);
static void echoCommand(redisClient *c);
static void setCommand(redisClient *c);
static void setnxCommand(redisClient *c);
static void getCommand(redisClient *c);
static void delCommand(redisClient *c);
static void existsCommand(redisClient *c);
static void incrCommand(redisClient *c);
static void decrCommand(redisClient *c);
static void incrbyCommand(redisClient *c);
static void decrbyCommand(redisClient *c);
static void selectCommand(redisClient *c);
static void randomkeyCommand(redisClient *c);
static void keysCommand(redisClient *c);
static void dbsizeCommand(redisClient *c);
static void lastsaveCommand(redisClient *c);
static void saveCommand(redisClient *c);
static void bgsaveCommand(redisClient *c);
static void shutdownCommand(redisClient *c);
static void moveCommand(redisClient *c);
static void renameCommand(redisClient *c);
static void renamenxCommand(redisClient *c);
static void lpushCommand(redisClient *c);
static void rpushCommand(redisClient *c);
static void lpopCommand(redisClient *c);
static void rpopCommand(redisClient *c);
static void llenCommand(redisClient *c);
static void lindexCommand(redisClient *c);
static void lrangeCommand(redisClient *c);
static void ltrimCommand(redisClient *c);
static void typeCommand(redisClient *c);
static void lsetCommand(redisClient *c);
static void saddCommand(redisClient *c);
static void sremCommand(redisClient *c);
static void smoveCommand(redisClient *c);
static void sismemberCommand(redisClient *c);
static void scardCommand(redisClient *c);
static void spopCommand(redisClient *c);
static void sinterCommand(redisClient *c);
static void sinterstoreCommand(redisClient *c);
static void sunionCommand(redisClient *c);
static void sunionstoreCommand(redisClient *c);
static void sdiffCommand(redisClient *c);
static void sdiffstoreCommand(redisClient *c);
static void syncCommand(redisClient *c);
static void flushdbCommand(redisClient *c);
static void flushallCommand(redisClient *c);
static void sortCommand(redisClient *c);
static void lremCommand(redisClient *c);
static void infoCommand(redisClient *c);
static void mgetCommand(redisClient *c);
static void monitorCommand(redisClient *c);
static void expireCommand(redisClient *c);
static void getSetCommand(redisClient *c);
static void ttlCommand(redisClient *c);
static void slaveofCommand(redisClient *c);
static void debugCommand(redisClient *c);
/*================================= Globals ================================= */

/* Global vars */
/*
typedef void redisCommandProc(redisClient *c);
struct redisCommand {
    char *name;//命令的名称，是一个以 null 结尾的字符串。例如，对于 SET 命令，这个成员的值将是 "SET"。
    redisCommandProc *proc;//该函数指针指向处理该命令的函数。
    int arity;//命令的参数个数。Redis 命令可以接受不同数量的参数，arity 成员指定了该命令必须接收的参数数量
    //一个标志位字段，用于存储与该命令相关的各种标志。这些标志可以指示命令的性质，比如命令是否可以在脚本中执行、
    //是否可以在订阅/发布通道上执行、是否需要加载数据库等。Redis 使用位掩码的方式来设置和检查这些标志。
    int flags;
};
 */
static struct redisServer server; /* server global state */
static struct redisCommand cmdTable[] = {
    {"get",getCommand,2,REDIS_CMD_INLINE},//获取一个key保存的值
    {"set",setCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//插入一个string 允许重复
    {"setnx",setnxCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//插入一个string 不允许重复
    {"del",delCommand,-2,REDIS_CMD_INLINE},//删除
    {"exists",existsCommand,2,REDIS_CMD_INLINE},//存在
    {"incr",incrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//+1
    {"decr",decrCommand,2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//-1
    {"mget",mgetCommand,-2,REDIS_CMD_INLINE},////获取多个值
    {"rpush",rpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//rpush
    {"lpush",lpushCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//lpush
    {"rpop",rpopCommand,2,REDIS_CMD_INLINE},//rpop
    {"lpop",lpopCommand,2,REDIS_CMD_INLINE},//lpop
    {"llen",llenCommand,2,REDIS_CMD_INLINE},////list长度
    {"lindex",lindexCommand,3,REDIS_CMD_INLINE},//获取list第index个
    {"lset",lsetCommand,4,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//列表的指定索引位置设置一个新元素。
    {"lrange",lrangeCommand,4,REDIS_CMD_INLINE},////在list中获取下标left-right的值
    {"ltrim",ltrimCommand,4,REDIS_CMD_INLINE},////用法 ltrim list num1 num2 只保留list num1-num2的内容
    {"lrem",lremCommand,4,REDIS_CMD_BULK},//list去掉 n 个 x
    {"sadd",saddCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},////set中增加一个key
    {"srem",sremCommand,3,REDIS_CMD_BULK},////删除key
    {"smove",smoveCommand,4,REDIS_CMD_BULK},////将set a中元素x移到set b
    {"sismember",sismemberCommand,3,REDIS_CMD_BULK},//查看set a中是不是有元素x
    {"scard",scardCommand,2,REDIS_CMD_INLINE},////求s的大小
    {"spop",spopCommand,2,REDIS_CMD_INLINE},//SPOP命令用于从集合中随机移除一个元素
    {"sinter",sinterCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//求多个集合交集
    {"sinterstore",sinterstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//求多个集合交集并存储
    {"sunion",sunionCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//交集
    {"sunionstore",sunionstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//交集存储
    {"sdiff",sdiffCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//差集
    {"sdiffstore",sdiffstoreCommand,-3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//差集存储
    {"smembers",sinterCommand,2,REDIS_CMD_INLINE},//交集
    {"incrby",incrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//+n
    {"decrby",decrbyCommand,3,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},//-n
    {"getset",getSetCommand,3,REDIS_CMD_BULK|REDIS_CMD_DENYOOM},//get and set
    {"randomkey",randomkeyCommand,1,REDIS_CMD_INLINE},
    {"select",selectCommand,2,REDIS_CMD_INLINE},//选择数据
    {"move",moveCommand,3,REDIS_CMD_INLINE},////从当前库拿出一个key放到指定的库
    {"rename",renameCommand,3,REDIS_CMD_INLINE},////重命名
    {"renamenx",renamenxCommand,3,REDIS_CMD_INLINE},//重命名
    {"expire",expireCommand,3,REDIS_CMD_INLINE},//增加过期时间
    {"keys",keysCommand,2,REDIS_CMD_INLINE},//查找符合正则表达式的key
    {"dbsize",dbsizeCommand,1,REDIS_CMD_INLINE},//数据库大小
    {"auth",authCommand,2,REDIS_CMD_INLINE},
    {"ping",pingCommand,1,REDIS_CMD_INLINE},//ping pong
    {"echo",echoCommand,2,REDIS_CMD_BULK},//回显
    {"save",saveCommand,1,REDIS_CMD_INLINE},//保存rdb
    {"bgsave",bgsaveCommand,1,REDIS_CMD_INLINE},////新进程保存rdb
    {"shutdown",shutdownCommand,1,REDIS_CMD_INLINE},
    {"lastsave",lastsaveCommand,1,REDIS_CMD_INLINE},//上一次存储时间
    {"type",typeCommand,2,REDIS_CMD_INLINE},////查看key类型
    {"sync",syncCommand,1,REDIS_CMD_INLINE},
    {"flushdb",flushdbCommand,1,REDIS_CMD_INLINE},//清空数据库
    {"flushall",flushallCommand,1,REDIS_CMD_INLINE},//清空所有数据库
    {"sort",sortCommand,-2,REDIS_CMD_INLINE|REDIS_CMD_DENYOOM},
    {"info",infoCommand,1,REDIS_CMD_INLINE},////打印一些日志
    {"monitor",monitorCommand,1,REDIS_CMD_INLINE},
    {"ttl",ttlCommand,2,REDIS_CMD_INLINE},////返回过期时间
    {"slaveof",slaveofCommand,3,REDIS_CMD_INLINE},
    {"debug",debugCommand,-2,REDIS_CMD_INLINE},//debug
    {NULL,NULL,0,0}
};
/*============================ Utility functions ============================ */

/* Glob-style pattern matching. */
int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase)//nocase控制大小写敏感
{
    while(patternLen) {
        switch(pattern[0]) {
        case '*':
            while (pattern[1] == '*') {
                pattern++;
                patternLen--;
            }
            if (patternLen == 1)
                return 1; /* match */
            //尝试匹配string,如果匹配失败跳过第一个字符继续匹配，直到完全失败
            while(stringLen) {
                if (stringmatchlen(pattern+1, patternLen-1,
                            string, stringLen, nocase))
                    return 1; /* match */
                string++;
                stringLen--;
            }
            return 0; /* no match */
            break;
        case '?':
        //如果遇到 ?，则检查字符串是否还有剩余字符。
        //如果有，则继续匹配下一个字符；如果没有，则返回 0
            if (stringLen == 0)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        case '[':
        {
        //string只要和[]里面内容一个符合就行
            int not, match;

            pattern++;
            patternLen--;
            not = pattern[0] == '^';
            if (not) {
                pattern++;
                patternLen--;
            }
            match = 0;
            while(1) {
                if (pattern[0] == '\\') {
                    pattern++;
                    patternLen--;
                    //跳过转义/并且判断/后面的字符是不是与string匹配
                    if (pattern[0] == string[0])
                        match = 1;
                } else if (pattern[0] == ']') {
                    break;
                } else if (patternLen == 0) {
                    pattern--;
                    patternLen++;
                    break;
                } else if (pattern[1] == '-' && patternLen >= 3) {
                    int start = pattern[0];
                    int end = pattern[2];
                    int c = string[0];
                    if (start > end) {
                        int t = start;
                        start = end;
                        end = t;
                    }
                    if (nocase) {
                        start = tolower(start);
                        end = tolower(end);
                        c = tolower(c);
                    }
                    pattern += 2;
                    patternLen -= 2;
                    if (c >= start && c <= end)
                        match = 1;
                } else {
                    if (!nocase) {
                        if (pattern[0] == string[0])
                            match = 1;
                    } else {
                        if (tolower((int)pattern[0]) == tolower((int)string[0]))
                            match = 1;
                    }
                }
                pattern++;
                patternLen--;
            }
            if (not)
                match = !match;
            if (!match)
                return 0; /* no match */
            string++;
            stringLen--;
            break;
        }
        case '\\':
        //如果遇到 \，则跳过它并处理其后的字符作为普通字符
            if (patternLen >= 2) {
                pattern++;
                patternLen--;
            }
            /* fall through */
        default:
        //对于非通配符、非转义字符，直接比较字符串和模式的当前字符。
        //如果 nocase 为真，则进行大小写不敏感的匹配。
        //如果不匹配，则返回 0（匹配失败）
            if (!nocase) {
                if (pattern[0] != string[0])
                    return 0; /* no match */
            } else {
                if (tolower((int)pattern[0]) != tolower((int)string[0]))
                    return 0; /* no match */
            }
            string++;
            stringLen--;
            break;
        }
        pattern++;
        patternLen--;
        //string匹配完成，看看pattern是不是后面都是*
        if (stringLen == 0) {
            while(*pattern == '*') {
                pattern++;
                patternLen--;
            }
            break;
        }
    }
    if (patternLen == 0 && stringLen == 0)
        return 1;
    return 0;
}

//将日志输出到文件或者打印到终端，如果server.logfile不存在则打印到终端
static void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*";
        char buf[64];
        time_t now;

        now = time(NULL);
        strftime(buf,64,"%d %b %H:%M:%S",gmtime(&now));
        fprintf(fp,"%s %c ",buf,c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp,"\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile) fclose(fp);
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

//判断key1和key2是否相等
static int sdsDictKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}
//单纯调用了decrRefCount，引用计数减1
static void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    decrRefCount(val);
}

//robj中保存的ptr为dict，判断两个dict是否相等
static int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return sdsDictKeyCompare(privdata,o1->ptr,o2->ptr);
}
//计算rojb->ptr的hash
static unsigned int dictSdsHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

static dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

static dictType hashDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
    fprintf(stderr, "%s: Out of memory\n",msg);
    fflush(stderr);
    sleep(1);
    abort();
}

/* ====================== Redis server networking stuff ===================== */
//这个函数 closeTimedoutClients 的目的是关闭那些超过最大空闲时间（server.maxidletime）的客户端连接，
//但会排除掉从服务器（slave）和主服务器（master）的连接，因为对于这两种类型的连接，通常不会设置空闲超时。
//这个函数在 Redis 服务器中周期性地被调用，以确保不会长时间保持无活动的客户端连接，从而节省系统资源
static void closeTimedoutClients(void) {
    redisClient *c;
    listNode *ln;
    time_t now = time(NULL);
    //从server的clients列表中去掉长时间保持无活动的客户端
    listRewind(server.clients);
    while ((ln = listYield(server.clients)) != NULL) {
        c = listNodeValue(ln);
        if (!(c->flags & REDIS_SLAVE) &&    /* no timeout for slaves */
            !(c->flags & REDIS_MASTER) &&   /* no timeout for masters */
             (now - c->lastinteraction > server.maxidletime)) {
            redisLog(REDIS_DEBUG,"Closing idle client");
            freeClient(c);
        }
    }
}
//判断是否需要重新变换dict的大小
static int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
static void tryResizeHashTables(void) {
    int j;
    //server.dbnum数据库数量
    for (j = 0; j < server.dbnum; j++) {
        //redisDb *db;
        //typedef struct redisDb {
        //dict *dict;//存储数据库中的所有键值对。
        //dict *expires;//存储设置了过期时间的键
        //int id;
        //} redisDb;
        if (htNeedsResize(server.db[j].dict)) {
            redisLog(REDIS_DEBUG,"The hash table %d is too sparse, resize it...",j);
            dictResize(server.db[j].dict);
            redisLog(REDIS_DEBUG,"Hash table %d resized.",j);
        }
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
    }
}

//定时器函数 aeCreateTimeEvent(server.el, 1000, serverCron, NULL, NULL);
static int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops++;//定时任务处理函数运行的次数。
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* Update the global state with the amount of used memory */
    server.usedmemory = zmalloc_used_memory();

    /* Show some info about non-empty databases */
    //将所有数据库里面的信息打印到日志
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;

        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 5) && (used || vkeys)) {
            redisLog(REDIS_DEBUG,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
            /* dictPrintStats(server.dict); */
        }
    }

    /*我们不想在后台保存时调整哈希表的大小
    *正在进行中：使用fork（）创建保存子对象，即
    *在大多数现代系统中，采用写时复制语义实现，因此
    *如果我们在保存子项实际工作时调整HT的大小
    *父级中的大量内存移动会导致大量页面
    *复制*/
    /* We don't want to resize the hash tables while a bacground saving
     * is in progress: the saving child is created using fork() that is
     * implemented with a copy-on-write semantic in most modern systems, so
     * if we resize the HT while there is the saving child at work actually
     * a lot of memory movements in the parent will cause a lot of pages
     * copied. */
    if (!server.bgsaveinprogress) tryResizeHashTables();

    /* Show information about connected clients */
    if (!(loops % 5)) {
        redisLog(REDIS_DEBUG,"%d clients connected (%d slaves), %zu bytes in use, %d shared objects",
            listLength(server.clients)-listLength(server.slaves),
            listLength(server.slaves),
            server.usedmemory,
            dictSize(server.sharingpool));
    }

    /* Close connections of timedout clients */
    //如果配置了最大空闲时间（server.maxidletime），并且满足循环次数条件（loops % 10），
    //则调用 closeTimedoutClients() 函数关闭那些超过最大空闲时间的客户端连接。
    if (server.maxidletime && !(loops % 10))
        closeTimedoutClients();

    /* Check if a background saving in progress terminated */
    if (server.bgsaveinprogress) {
        int statloc;
        //通过调用wait4函数（参数-1表示等待任意子进程，&statloc用于存储子进程的退出状态，
        //WNOHANG选项表示如果没有子进程退出，则立即返回），尝试检查是否有后台保存的子进程已经结束。
        if (wait4(-1,&statloc,WNOHANG,NULL)) {
            //如果wait4返回非零值（即确实有子进程退出），则通过WEXITSTATUS(statloc)
            //获取子进程的退出码，通过WIFSIGNALED(statloc)检查子进程是否因接收到信号而退出。
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = WIFSIGNALED(statloc);

            if (!bysignal && exitcode == 0) {
                redisLog(REDIS_NOTICE,
                    "Background saving terminated with success");
                //刚保存完，所以没有change
                server.dirty = 0;
                server.lastsave = time(NULL);
            } else if (!bysignal && exitcode != 0) {
                redisLog(REDIS_WARNING, "Background saving error");
            } else {
                redisLog(REDIS_WARNING,
                    "Background saving terminated by signal");
                rdbRemoveTempFile(server.bgsavechildpid);
            }
            server.bgsaveinprogress = 0;
            server.bgsavechildpid = -1;//后台保存进程
            //根据后台保存是否成功（通过exitcode == 0 ? REDIS_OK : REDIS_ERR判断），
            //来更新那些可能正在等待主服务器完成后台保存操作以进行同步的从服务器的状态。
            updateSlavesWaitingBgsave(exitcode == 0 ? REDIS_OK : REDIS_ERR);
        }
    } else {
        /* If there is not a background saving in progress check if
         * we have to save now */
         /*
        *struct saveparam {
        *time_t seconds;
        *int changes;
        *};  
         */
         time_t now = time(NULL);
         for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            if (server.dirty >= sp->changes &&
                now-server.lastsave > sp->seconds) {
                redisLog(REDIS_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, sp->seconds);
                rdbSaveBackground(server.dbfilename);
                break;
            }
         }
    }

    /* Try to expire a few timed out keys */
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        int num = dictSize(db->expires);

        if (num) {
            time_t now = time(NULL);

            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                dictEntry *de;
                time_t t;
                //随机获取一个expire然后如果超时则删除
                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                t = (time_t) dictGetEntryVal(de);
                if (now > t) {
                    deleteKey(db,dictGetEntryKey(de));
                }
            }
        }
    }

    /* Check if we should connect to a MASTER */
    if (server.replstate == REDIS_REPL_CONNECT) {
        redisLog(REDIS_NOTICE,"Connecting to MASTER...");
        if (syncWithMaster() == REDIS_OK) {
            redisLog(REDIS_NOTICE,"MASTER <-> SLAVE sync succeeded");
        }
    }
    return 1000;
}
//用于初始化
static void createSharedObjects(void) {
    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING,sdsnew("*0\r\n"));
    /* no such key */
    shared.pong = createObject(REDIS_STRING,sdsnew("+PONG\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(REDIS_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(REDIS_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(REDIS_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.space = createObject(REDIS_STRING,sdsnew(" "));
    shared.colon = createObject(REDIS_STRING,sdsnew(":"));
    shared.plus = createObject(REDIS_STRING,sdsnew("+"));
    shared.select0 = createStringObject("select 0\r\n",10);
    shared.select1 = createStringObject("select 1\r\n",10);
    shared.select2 = createStringObject("select 2\r\n",10);
    shared.select3 = createStringObject("select 3\r\n",10);
    shared.select4 = createStringObject("select 4\r\n",10);
    shared.select5 = createStringObject("select 5\r\n",10);
    shared.select6 = createStringObject("select 6\r\n",10);
    shared.select7 = createStringObject("select 7\r\n",10);
    shared.select8 = createStringObject("select 8\r\n",10);
    shared.select9 = createStringObject("select 9\r\n",10);
}
/*
*struct saveparam {
*    time_t seconds;
*    int changes;
*};

*/
//这段代码的主要功能是在Redis服务器配置中动态地添加一个新的保存参数
static void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));
    if (server.saveparams == NULL) oom("appendServerSaveParams");
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}
//释放保存参数
static void ResetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;//16
    server.port = REDIS_SERVERPORT;//6379
    server.verbosity = REDIS_DEBUG;//日志级别 0
    server.maxidletime = REDIS_MAXIDLETIME;//default client timeout 5*60
    server.saveparams = NULL;
    server.logfile = NULL; /* NULL = log on standard output */
    server.bindaddr = NULL;
    server.glueoutputbuf = 1;////是否将输出合并
    server.daemonize = 0;//是否作为守护进程运行
    server.pidfile = "/var/run/redis.pid";
    server.dbfilename = "dump.rdb";
    server.requirepass = NULL;///密码
    server.shareobjects = 0;////是否共享对象
    server.sharingpoolsize = 1024;//对象共享池大小
    server.maxclients = 0;//服务器允许的最大客户端连接数
    server.maxmemory = 0;////服务器允许使用的最大内存量
    ResetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */
    /* Replication related */
    server.isslave = 0;//指示当前服务器是否是一个从服务器。
    server.masterhost = NULL;////主服务器的地址
    server.masterport = 6379;
    server.master = NULL;
    server.replstate = REDIS_REPL_NONE;////复制状态。 No active replication
}

static void initServer() {
    int j;
//SIGHUP（Signal Hang Up）是一个信号，通常用于通知用户终端已经断开连接。在守护进程（daemon）和其他长时间运行的程序中，
//SIGHUP信号通常用于要求程序重新加载其配置文件或执行某些清理操作。SIG_IGN是一个特殊的宏，表示忽略该信号。
    signal(SIGHUP, SIG_IGN);
//SIGPIPE是一个信号，当向一个已经关闭其读取端的管道（pipe）或套接字（socket）写入数据时产生。默认情况下，当发生SIGPIPE信号时，进程会终止。
    signal(SIGPIPE, SIG_IGN);
    //为几个关键的信号（段错误、总线错误、浮点异常和非法指令）设置信号处理函数。
    setupSigSegvAction();
    //创建双向链表
    server.clients = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.objfreelist = listCreate();
    createSharedObjects();//初始化shared
    server.el = aeCreateEventLoop(200);//创建事件循环
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    server.sharingpool = dictCreate(&setDictType,NULL);
    if (!server.db || !server.clients || !server.slaves || !server.monitors || !server.el || !server.objfreelist)
        oom("server initialization"); /* Fatal OOM */
    server.fd = anetTcpServer(server.neterr, server.port, server.bindaddr);
    if (server.fd == -1) {
        redisLog(REDIS_WARNING, "Opening TCP port: %s", server.neterr);
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&hashDictType,NULL);
        server.db[j].expires = dictCreate(&setDictType,NULL);
        server.db[j].id = j;
    }
    server.cronloops = 0;//函数（Redis 的定时任务处理函数）运行的次数。
    server.bgsaveinprogress = 0;//是否正在进行后台保存
    server.bgsavechildpid = -1;////后台保存子进程的 PID
    server.lastsave = time(NULL);//上次保存时间
    server.dirty = 0;////自上次保存数据库以来，数据库被修改的次数
    server.usedmemory = 0;//使用的内存
    server.stat_numcommands = 0;//服务器处理过的命令总数
    server.stat_numconnections = 0;//服务器接收到的连接总数
    server.stat_starttime = time(NULL);
    aeCreateTimeEvent(server.el, 1000, serverCron, NULL, NULL);
}

/* Empty the whole database */
static long long emptyDb() {
    int j;
    long long removed = 0;

    for (j = 0; j < server.dbnum; j++) {
        removed += dictSize(server.db[j].dict);////获取目前hash表中有多少条记录
        dictEmpty(server.db[j].dict);//清空
        dictEmpty(server.db[j].expires);
    }
    return removed;
}
//不区分大小写
static int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

/* I agree, this is a very rudimental way to load a configuration...
   will improve later if the config gets more complex */
static void loadServerConfig(char *filename) {
    FILE *fp;
    char buf[REDIS_CONFIGLINE_MAX+1], *err = NULL;//REDIS_CONFIGLINE_MAX=1024
    int linenum = 0;
    sds line = NULL;

    if (filename[0] == '-' && filename[1] == '\0')
        fp = stdin;
    else {
        if ((fp = fopen(filename,"r")) == NULL) {
            redisLog(REDIS_WARNING,"Fatal error, can't open config file");
            exit(1);
        }
    }

    while(fgets(buf,REDIS_CONFIGLINE_MAX+1,fp) != NULL) {
        sds *argv;
        int argc, j;

        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line," \t\r\n");//去掉字符串两边指定的字符

        /* Skip comments and blank lines*/
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }

        /* Split into arguments */
        argv = sdssplitlen(line,sdslen(line)," ",1,&argc);
        sdstolower(argv[0]);

        /* Execute config directives */
        if (!strcasecmp(argv[0],"timeout") && argc == 2) {//最大空闲时间
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"port") && argc == 2) {//端口
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"bind") && argc == 2) {//ip
            server.bindaddr = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"save") && argc == 3) {
            int seconds = atoi(argv[1]);
            int changes = atoi(argv[2]);
            if (seconds < 1 || changes < 0) {
                err = "Invalid save parameters"; goto loaderr;
            }
            appendServerSaveParams(seconds,changes);//保存参数设置
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"loglevel") && argc == 2) {
            if (!strcasecmp(argv[1],"debug")) server.verbosity = REDIS_DEBUG;
            else if (!strcasecmp(argv[1],"notice")) server.verbosity = REDIS_NOTICE;
            else if (!strcasecmp(argv[1],"warning")) server.verbosity = REDIS_WARNING;
            else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;

            server.logfile = zstrdup(argv[1]);
            if (!strcasecmp(server.logfile,"stdout")) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile) {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"databases") && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"slaveof") && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.replstate = REDIS_REPL_CONNECT;
        } else if (!strcasecmp(argv[0],"glueoutputbuf") && argc == 2) {
            if ((server.glueoutputbuf = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"shareobjects") && argc == 2) {//是否共享对象
            if ((server.shareobjects = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"shareobjectspoolsize") && argc == 2) {//对象共享池大小
            server.sharingpoolsize = atoi(argv[1]);
            if (server.sharingpoolsize < 1) {
                err = "invalid object sharing pool size"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {//守护进程
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {////密码
          server.requirepass = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"pidfile") && argc == 2) {
          server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
          server.dbfilename = zstrdup(argv[1]);
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        for (j = 0; j < argc; j++)
            sdsfree(argv[j]);
        zfree(argv);
        sdsfree(line);
    }
    if (fp != stdin) fclose(fp);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", line);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

static void freeClientArgv(redisClient *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);////argv为指向命令参数数组的指针。当 Redis 解析了客户端发送的命令后，它会将命令的参数存储在这个数组中。
    c->argc = 0;
}

//没看完
static void freeClient(redisClient *c) {
    listNode *ln;
    //取消监听客户端的读和写事件
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    //用于存储客户端发送的查询命令的缓冲区。
    sdsfree(c->querybuf);
    ////指向回复队列的指针。Redis 会将命令的回复添加到这个队列中，然后通过套接字发送给客户端。
    listRelease(c->reply);
    ////已经发送给客户端的回复字节数。这个值用于跟踪回复的发送进度，特别是在处理大回复时。
    freeClientArgv(c);
    close(c->fd);
    ln = listSearchKey(server.clients,c);
    assert(ln != NULL);
    listDelNode(server.clients,ln);
    if (c->flags & REDIS_SLAVE) {//This client is a slave server
        if (c->replstate == REDIS_REPL_SEND_BULK && c->repldbfd != -1)////复制数据库文件描述符。
            close(c->repldbfd);
        list *l = (c->flags & REDIS_MONITOR) ? server.monitors : server.slaves;
        ln = listSearchKey(l,c);
        assert(ln != NULL);
        listDelNode(l,ln);
    }
    if (c->flags & REDIS_MASTER) {
        server.master = NULL;
        server.replstate = REDIS_REPL_CONNECT;
    }
    zfree(c->argv);
    zfree(c);
}
//在需要将多个小的回复缓冲区（robj 对象，通常包含字符串数据）
//合并成一个大的缓冲区时进行优化。这通常是为了减少在将回复发送给客户端时的系统调用次数，
//从而提高性能。
static void glueReplyBuffersIfNeeded(redisClient *c) {
    int totlen = 0;
    listNode *ln;
    robj *o;

    listRewind(c->reply);//将链表结构中内含的迭代器设置为向尾遍历
    while((ln = listYield(c->reply))) {//放弃当前访问的节点，返回下一个节点的值
        o = ln->value;
        totlen += sdslen(o->ptr);
        /* This optimization makes more sense if we don't have to copy
         * too much data */
        if (totlen > 1024) return;
    }
    if (totlen > 0) {
        char buf[1024];
        int copylen = 0;

        listRewind(c->reply);
        while((ln = listYield(c->reply))) {
            o = ln->value;
            memcpy(buf+copylen,o->ptr,sdslen(o->ptr));
            copylen += sdslen(o->ptr);
            listDelNode(c->reply,ln);
        }
        /* Now the output buffer is empty, add the new single element */
        o = createObject(REDIS_STRING,sdsnewlen(buf,totlen));
        if (!listAddNodeTail(c->reply,o)) oom("listAddNodeTail");
    }
}

static void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    //server.glueoutputbuf表示是否使用粘包
    if (server.glueoutputbuf && listLength(c->reply) > 1)
        glueReplyBuffersIfNeeded(c);
    while(listLength(c->reply)) {
        o = listNodeValue(listFirst(c->reply));
        objlen = sdslen(o->ptr);

        if (objlen == 0) {
            listDelNode(c->reply,listFirst(c->reply));
            continue;
        }

        if (c->flags & REDIS_MASTER) {
            /* Don't reply to a master */
            nwritten = objlen - c->sentlen;
        } else {
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen, objlen - c->sentlen);
            if (nwritten <= 0) break;
        }
        c->sentlen += nwritten;
        totwritten += nwritten;
        /* If we fully sent the object on head go to the next one */
        if (c->sentlen == objlen) {
            listDelNode(c->reply,listFirst(c->reply));
            c->sentlen = 0;
        }
        /* Note that we avoid to send more thank REDIS_MAX_WRITE_PER_EVENT
         * bytes, in a single threaded server it's a good idea to server
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * terms think to 'KEYS *' against the loopback interfae) */
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT) break;
    }
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_DEBUG,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    if (totwritten > 0) c->lastinteraction = time(NULL);
    if (listLength(c->reply) == 0) {
        c->sentlen = 0;
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
    }
}
//查找命令，以及关联函数
static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while(cmdTable[j].name != NULL) {
        if (!strcasecmp(name,cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

/* resetClient prepare the client to process the next command */
//重置client处理下一条命令
static void resetClient(redisClient *c) {
    freeClientArgv(c);
    c->bulklen = -1;
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
static int processCommand(redisClient *c) {
    struct redisCommand *cmd;
    long long dirty;

    /* Free some memory if needed (maxmemory setting) */
    //如果服务器配置了最大内存限制（server.maxmemory），则调用 freeMemoryIfNeeded 函数来释放足够的内存，以满足内存使用不超过限制。
    if (server.maxmemory) freeMemoryIfNeeded();

    /* The QUIT command is handled as a special case. Normal command
     * procs are unable to close the client connection safely */
    //特殊情况处理，如果客户端请求的是 QUIT 命令，则直接调用 freeClient 函数关闭客户端连接，并返回 0。
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        freeClient(c);
        return 0;
    }
    cmd = lookupCommand(c->argv[0]->ptr);
    if (!cmd) {
        addReplySds(c,sdsnew("-ERR unknown command\r\n"));
        resetClient(c);
        return 1;
    } else if ((cmd->arity > 0 && cmd->arity != c->argc) ||
               (c->argc < -cmd->arity)) {
        addReplySds(c,sdsnew("-ERR wrong number of arguments\r\n"));
        resetClient(c);
        return 1;
    } else if (server.maxmemory && cmd->flags & REDIS_CMD_DENYOOM && zmalloc_used_memory() > server.maxmemory) {
        addReplySds(c,sdsnew("-ERR command not allowed when used memory > 'maxmemory'\r\n"));
        resetClient(c);
        return 1;
    } else if (cmd->flags & REDIS_CMD_BULK && c->bulklen == -1) {
        //从最后一位读取长度
        int bulklen = atoi(c->argv[c->argc-1]->ptr);
        //释放最后一个参数
        decrRefCount(c->argv[c->argc-1]);
        //接下来，它检查 bulklen 是否在有效范围内
        if (bulklen < 0 || bulklen > 1024*1024*1024) {
            c->argc--;
            addReplySds(c,sdsnew("-ERR invalid bulk write count\r\n"));
            resetClient(c);
            return 1;
        }
        c->argc--;
        c->bulklen = bulklen+2; /* add two bytes for CR+LF */
        /* It is possible that the bulk read is already in the
         * buffer. Check this condition and handle it accordingly */
        //这行代码从 c->querybuf 中提取前 c->bulklen-2 个字节作为批量数据（减去2是为了排除可能的尾随换行符），
        //并使用 createStringObject 函数创建一个新的字符串对象。然后，这个新创建的对象被存储在 c->argv 数组的 c->argc 索引处。
        if ((signed)sdslen(c->querybuf) >= c->bulklen) {
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            //c->querybuf减少
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
        } else {
            return 1;
        }
    }
    /* Let's try to share objects on the command arguments vector */
    if (server.shareobjects) {
        int j;
        for(j = 1; j < c->argc; j++)
            c->argv[j] = tryObjectSharing(c->argv[j]);
    }
    /* Check if the user is authenticated */
    if (server.requirepass && !c->authenticated && cmd->proc != authCommand) {
        addReplySds(c,sdsnew("-ERR operation not permitted\r\n"));
        resetClient(c);
        return 1;
    }

    /* Exec the command */
    //执行命令，如果存在dirty则把命令发给从服务器和监视服务器
    dirty = server.dirty;
    cmd->proc(c);
    if (server.dirty-dirty != 0 && listLength(server.slaves))
        replicationFeedSlaves(server.slaves,cmd,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedSlaves(server.monitors,cmd,c->db->id,c->argv,c->argc);
    server.stat_numcommands++;

    /* Prepare the client for the next command */
    if (c->flags & REDIS_CLOSE) {
        freeClient(c);
        return 0;
    }
    resetClient(c);
    return 1;
}
//replicationFeedSlaves 函数是 Redis 复制功能的一部分，
//用于将命令及其参数复制给所有处于活动状态的从服务器（slave）。这个函数处理命令的序列化，确保它们可以被从服务器正确解析和执行。下面是该函数的详细解释：
static void replicationFeedSlaves(list *slaves, struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    listNode *ln;
    int outc = 0, j;
    robj **outv;
    /* (args*2)+1 is enough room for args, spaces, newlines */
    robj *static_outv[REDIS_STATIC_ARGS*2+1];
//首先，根据命令参数的数量（argc），函数决定是使用静态分配的数组static_outv还是动态分配一个更大的数组outv来存储输出对象。
//如果参数数量不超过REDIS_STATIC_ARGS，则使用静态数组以节省内存。否则，动态分配足够的内存。
    if (argc <= REDIS_STATIC_ARGS) {
        outv = static_outv;
    } else {
        outv = zmalloc(sizeof(robj*)*(argc*2+1));
        if (!outv) oom("replicationFeedSlaves");
    }

    for (j = 0; j < argc; j++) {
        if (j != 0) outv[outc++] = shared.space;
        if ((cmd->flags & REDIS_CMD_BULK) && j == argc-1) {
            robj *lenobj;

            lenobj = createObject(REDIS_STRING,
                sdscatprintf(sdsempty(),"%d\r\n",sdslen(argv[j]->ptr)));
            lenobj->refcount = 0;
            outv[outc++] = lenobj;
        }
        outv[outc++] = argv[j];
    }
    outv[outc++] = shared.crlf;

    /* Increment all the refcounts at start and decrement at end in order to
     * be sure to free objects if there is no slave in a replication state
     * able to be feed with commands */
    for (j = 0; j < outc; j++) incrRefCount(outv[j]);
    listRewind(slaves);
    while((ln = listYield(slaves))) {
        redisClient *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start */
        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) continue;

        /* Feed all the other slaves, MONITORs and so on */
        if (slave->slaveseldb != dictid) {
            robj *selectcmd;

            switch(dictid) {
            case 0: selectcmd = shared.select0; break;
            case 1: selectcmd = shared.select1; break;
            case 2: selectcmd = shared.select2; break;
            case 3: selectcmd = shared.select3; break;
            case 4: selectcmd = shared.select4; break;
            case 5: selectcmd = shared.select5; break;
            case 6: selectcmd = shared.select6; break;
            case 7: selectcmd = shared.select7; break;
            case 8: selectcmd = shared.select8; break;
            case 9: selectcmd = shared.select9; break;
            default:
                selectcmd = createObject(REDIS_STRING,
                    sdscatprintf(sdsempty(),"select %d\r\n",dictid));
                selectcmd->refcount = 0;
                break;
            }
            addReply(slave,selectcmd);
            slave->slaveseldb = dictid;
        }
        for (j = 0; j < outc; j++) addReply(slave,outv[j]);
    }
    for (j = 0; j < outc; j++) decrRefCount(outv[j]);
    if (outv != static_outv) zfree(outv);
}

static void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    char buf[REDIS_IOBUF_LEN];
    int nread;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    nread = read(fd, buf, REDIS_IOBUF_LEN);
    if (nread == -1) {
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_DEBUG, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {
        redisLog(REDIS_DEBUG, "Client closed connection");
        freeClient(c);
        return;
    }
    if (nread) {
        //进行字符串拼接
        c->querybuf = sdscatlen(c->querybuf, buf, nread);
        c->lastinteraction = time(NULL);
    } else {
        return;
    }

again:
    //如果没有进行批处理，度取出一行，将参数分解出来，放到argc和argv中，并处理参数
    //如果每处理完，这继续运行到again的位置
    if (c->bulklen == -1) {
        /* Read the first line of the query */
        char *p = strchr(c->querybuf,'\n');
        size_t querylen;

        if (p) {
            sds query, *argv;
            int argc, j;

            query = c->querybuf;
            c->querybuf = sdsempty();
            querylen = 1+(p-(query));
            if (sdslen(query) > querylen) {//c->querybuf指向提取出数据后面的部分
                /* leave data after the first line of the query in the buffer */
                c->querybuf = sdscatlen(c->querybuf,query+querylen,sdslen(query)-querylen);
            }
            *p = '\0'; /* remove "\n" */
            if (*(p-1) == '\r') *(p-1) = '\0'; /* and "\r" if any */
            sdsupdatelen(query);

            /* Now we can split the query in arguments */
            if (sdslen(query) == 0) {
                /* Ignore empty query */
                sdsfree(query);
                return;
            }
            argv = sdssplitlen(query,sdslen(query)," ",1,&argc);
            if (argv == NULL) oom("sdssplitlen");
            sdsfree(query);

            if (c->argv) zfree(c->argv);
            c->argv = zmalloc(sizeof(robj*)*argc);
            if (c->argv == NULL) oom("allocating arguments list for client");

            for (j = 0; j < argc; j++) {
                if (sdslen(argv[j])) {
                    c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
                    c->argc++;
                } else {
                    sdsfree(argv[j]);
                }
            }
            zfree(argv);
            /* Execute the command. If the client is still valid
             * after processCommand() return and there is something
             * on the query buffer try to process the next command. */
            if (c->argc && processCommand(c) && sdslen(c->querybuf)) goto again;
            return;
        } else if (sdslen(c->querybuf) >= REDIS_REQUEST_MAX_SIZE) {
            redisLog(REDIS_DEBUG, "Client protocol error");
            freeClient(c);
            return;
        }
    } else {
        /* Bulk read handling. Note that if we are at this point
           the client already sent a command terminated with a newline,
           we are reading the bulk data that is actually the last
           argument of the command. */
        int qbl = sdslen(c->querybuf);

        if (c->bulklen <= qbl) {
            /* Copy everything but the final CRLF as final argument */
            c->argv[c->argc] = createStringObject(c->querybuf,c->bulklen-2);
            c->argc++;
            c->querybuf = sdsrange(c->querybuf,c->bulklen,-1);
            processCommand(c);
            return;
        }
    }
}
//选择数据库
static int selectDb(redisClient *c, int id) {
    if (id < 0 || id >= server.dbnum)
        return REDIS_ERR;
    c->db = &server.db[id];
    return REDIS_OK;
}
//引用计数+1
static void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return 0;
}

static redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(*c));
    /**
    * 将fd设置为非阻塞型
    */
    anetNonBlock(NULL,fd);
    /**
    * 将TCP设为非延迟的，即屏蔽Nagle算法
    */
    anetTcpNoDelay(NULL,fd);
    if (!c) return NULL;
    selectDb(c,0);
    c->fd = fd;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bulklen = -1;
    c->sentlen = 0;
    c->flags = 0;
    c->lastinteraction = time(NULL);
    c->authenticated = 0;//密码
    c->replstate = REDIS_REPL_NONE;//No active replication
    if ((c->reply = listCreate()) == NULL) oom("listCreate");
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    if (aeCreateFileEvent(server.el, c->fd, AE_READABLE,
        readQueryFromClient, c) == AE_ERR) {
        freeClient(c);
        return NULL;
    }
    
/*
 * 尾部插入节点的值
 */
    if (!listAddNodeTail(server.clients,c)) oom("listAddNodeTail");
    return c;
}
//注册一个客户端的可写事件
static void addReply(redisClient *c, robj *obj) {
    if (listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||//如果这个客户端是一个从服务器，这个字段将存储其复制状态
         c->replstate == REDIS_REPL_ONLINE) &&
        aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
        sendReplyToClient, c) == AE_ERR) return;
    //注册了可写事件，但是并没有调用select函数，所以不会立刻执行
    if (!listAddNodeTail(c->reply,obj)) oom("listAddNodeTail");
    incrRefCount(obj);
}

static void addReplySds(redisClient *c, sds s) {
    robj *o = createObject(REDIS_STRING,s);
    addReply(c,o);
    decrRefCount(o);
}
//连接一个客户端
static void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    char cip[128];
    redisClient *c;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    REDIS_NOTUSED(privdata);

    cfd = anetAccept(server.neterr, fd, cip, &cport);    //接收客户端连接
    if (cfd == AE_ERR) {
        redisLog(REDIS_DEBUG,"Accepting client connection: %s", server.neterr);
        return;
    }
    redisLog(REDIS_DEBUG,"Accepted %s:%d", cip, cport);    //收到客户端命令,比如  ./redis-cli set k1 v1
    if ((c = createClient(cfd)) == NULL) {
        redisLog(REDIS_WARNING,"Error allocating resoures for the client");
        close(cfd); /* May be already closed, just ingore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in nonblocking
     * mode and we can send an error for free using the Kernel I/O */
    if (server.maxclients && listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        (void) write(c->fd,err,strlen(err));
        freeClient(c);
        return;
    }
    server.stat_numconnections++;
}

/* ======================= Redis objects implementation ===================== */
//创建一个robj*对象
static robj *createObject(int type, void *ptr) {
    robj *o;

    if (listLength(server.objfreelist)) {
        listNode *head = listFirst(server.objfreelist);//获取链表的头结点
        o = listNodeValue(head);//获取链表当前节点的值
        listDelNode(server.objfreelist,head);//从objfreelist删除
    } else {
        o = zmalloc(sizeof(*o));
    }
    if (!o) oom("createObject");
    o->type = type;
    o->ptr = ptr;
    o->refcount = 1;
    return o;
}

static robj *createStringObject(char *ptr, size_t len) {
    return createObject(REDIS_STRING,sdsnewlen(ptr,len));
}

static robj *createListObject(void) {
    list *l = listCreate();

    if (!l) oom("listCreate");
    listSetFreeMethod(l,decrRefCount);
    return createObject(REDIS_LIST,l);
}

static robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);
    if (!d) oom("dictCreate");
    return createObject(REDIS_SET,d);
}
//robj的void *ptr;可以储存任意redis数据结构
//释放robj中的String对象
static void freeStringObject(robj *o) {
    sdsfree(o->ptr);
}
//释放robj中的String对象
static void freeListObject(robj *o) {
    listRelease((list*) o->ptr);
}
//释放哈希表
static void freeSetObject(robj *o) {
    dictRelease((dict*) o->ptr);
}
//释放哈希表
static void freeHashObject(robj *o) {
    dictRelease((dict*) o->ptr);
}
/*typedef struct redisObject {
    void *ptr;
    int type;
    int refcount;
} robj;
*/
//引用计数+1
static void incrRefCount(robj *o) {
    o->refcount++;
#ifdef DEBUG_REFCOUNT
    if (o->type == REDIS_STRING)
        printf("Increment '%s'(%p), now is: %d\n",o->ptr,o,o->refcount);
#endif
}
//引用计数字减1
static void decrRefCount(void *obj) {
    robj *o = obj;

#ifdef DEBUG_REFCOUNT
    if (o->type == REDIS_STRING)
        printf("Decrement '%s'(%p), now is: %d\n",o->ptr,o,o->refcount-1);
#endif
    if (--(o->refcount) == 0) {
        switch(o->type) {
        case REDIS_STRING: freeStringObject(o); break;
        case REDIS_LIST: freeListObject(o); break;
        case REDIS_SET: freeSetObject(o); break;
        case REDIS_HASH: freeHashObject(o); break;    //应该不会出现HASH类型
        default: assert(0 != 0); break;
        }
        //在释放对象后，Redis尝试将对象添加到一个名为server.objfreelist的列表中，以便后续重用。
        //如果server.objfreelist的长度超过了预设的最大值REDIS_OBJFREELIST_MAX，
        //或者由于某种原因无法将对象添加到列表中（!listAddNodeHead(server.objfreelist,o)返回非零值），则直接调用zfree(o)释放对象所占用的内存。
        if (listLength(server.objfreelist) > REDIS_OBJFREELIST_MAX ||
            !listAddNodeHead(server.objfreelist,o))
            zfree(o);
    }
}

/* Try to share an object against the shared objects pool */
//这段代码实现了一个简单的对象共享池机制，用于在Redis中减少内存使用，特别是针对字符串类型的对象。
//它通过维护一个字典（server.sharingpool）来跟踪哪些字符串对象已经被共享，并允许后续请求重用这些对象而不是创建新的对象。
static robj *tryObjectSharing(robj *o) {
    struct dictEntry *de;
    unsigned long c;

    if (o == NULL || server.shareobjects == 0) return o;

    assert(o->type == REDIS_STRING);    //o是REDIS_STRING类型
    de = dictFind(server.sharingpool,o);//sharingpool保存了o的使用次数
    if (de) {
        robj *shared = dictGetEntryKey(de);

        c = ((unsigned long) dictGetEntryVal(de))+1;
        dictGetEntryVal(de) = (void*) c;
        incrRefCount(shared);
        decrRefCount(o);
        return shared;
    } else {
        /* Here we are using a stream algorihtm: Every time an object is
         * shared we increment its count, everytime there is a miss we
         * recrement the counter of a random object. If this object reaches
         * zero we remove the object and put the current object instead. */
        if (dictSize(server.sharingpool) >=
                server.sharingpoolsize) {
            de = dictGetRandomKey(server.sharingpool);
            assert(de != NULL);
            c = ((unsigned long) dictGetEntryVal(de))-1;
            dictGetEntryVal(de) = (void*) c;
            if (c == 0) {
                dictDelete(server.sharingpool,de->key);
            }
        } else {
            c = 0; /* If the pool is empty we want to add this object */
        }
        if (c == 0) {
            int retval;

            retval = dictAdd(server.sharingpool,o,(void*)1);
            assert(retval == DICT_OK);
            incrRefCount(o);
        }
        return o;
    }
}
/*
    typedef struct redisDb {
    dict *dict;//存储数据库中的所有键值对。
    dict *expires;//存储设置了过期时间的键
    int id;
    } redisDb;
*/
static robj *lookupKey(redisDb *db, robj *key) {
    dictEntry *de = dictFind(db->dict,key);
    return de ? dictGetEntryVal(de) : NULL;
}
//查一个key如果过期则删除，然后查找
static robj *lookupKeyRead(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key);
}

static robj *lookupKeyWrite(redisDb *db, robj *key) {
    deleteIfVolatile(db,key);
    return lookupKey(db,key);
}

static int deleteKey(redisDb *db, robj *key) {
    int retval;

    /* We need to protect key from destruction: after the first dictDelete()
     * it may happen that 'key' is no longer valid if we don't increment
     * it's count. This may happen when we get the object reference directly
     * from the hash table with dictRandomKey() or dict iterators */
    incrRefCount(key);
    if (dictSize(db->expires)) dictDelete(db->expires,key);
    retval = dictDelete(db->dict,key);
    decrRefCount(key);

    return retval == DICT_OK;
}

/*============================ DB saving/loading ============================ */
//数据类型保存到文件
static int rdbSaveType(FILE *fp, unsigned char type) {
    if (fwrite(&type,1,1,fp) == 0) return -1;
    return 0;
}
//时间辍保存到文件
static int rdbSaveTime(FILE *fp, time_t t) {
    int32_t t32 = (int32_t) t;
    if (fwrite(&t32,4,1,fp) == 0) return -1;
    return 0;
}
//对不同长度uint32_t进行保存
/* check rdbLoadLen() comments for more info */
static int rdbSaveLen(FILE *fp, uint32_t len) {
    unsigned char buf[2];

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);       //00[低6位len]
        if (fwrite(buf,1,1,fp) == 0) return -1;           //写1个1字节
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(REDIS_RDB_14BITLEN<<6);    //高6位 01[高6位]
        buf[1] = len&0xFF;                                   //低8位 [ 低8位 ]
        if (fwrite(buf,2,1,fp) == 0) return -1;              //写1个2字节
    } else {
        /* Save a 32 bit len */
        buf[0] = (REDIS_RDB_32BITLEN<<6);                  //10000000
        if (fwrite(buf,1,1,fp) == 0) return -1;
        len = htonl(len);                                 //转网络大端序
        if (fwrite(&len,4,1,fp) == 0) return -1;
    }
    return 0;
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
 //用于尝试将一个字符串（SDS 类型，即 Redis 中的动态字符串）编码为一个整数，并确定其适合的整数编码类型（8位、16位、32位）
static int rdbTryIntegerEncoding(sds s, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    snprintf(buf,32,"%lld",value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
     //接下来，函数将转换得到的 value 再次转换回字符串，并与原 SDS 字符串进行比较。如果转换后的字符串长度与原 SDS 字符串长度不同，或者内容不完全一致，则返回 0
    if (strlen(buf) != sdslen(s) || memcmp(buf,s,sdslen(s))) return 0;
    //在确认原字符串可以安全地解释为整数后，函数接下来检查这个整数是否适合用 8 位、16 位或 32 位整数来表示。
    /* Finally check if it fits in our ranges */
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}
//用于将字符串对象（robj 类型的 obj）使用 LZF 压缩算法压缩后保存到文件中的函数。
static int rdbSaveLzfStringObject(FILE *fp, robj *obj) {
    unsigned int comprlen, outlen;
    unsigned char byte;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    outlen = sdslen(obj->ptr)-4;
    if (outlen <= 0) return 0;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(obj->ptr, sdslen(obj->ptr), out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    /* Data compressed! Let's save it on disk */
    byte = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_LZF;
    if (fwrite(&byte,1,1,fp) == 0) goto writeerr;
    if (rdbSaveLen(fp,comprlen) == -1) goto writeerr;
    if (rdbSaveLen(fp,sdslen(obj->ptr)) == -1) goto writeerr;
    if (fwrite(out,comprlen,1,fp) == 0) goto writeerr;
    zfree(out);
    return comprlen;

writeerr:
    zfree(out);
    return -1;
}

/* Save a string objet as [len][data] on disk. If the object is a string
 * representation of an integer value we try to safe it in a special form */
static int rdbSaveStringObject(FILE *fp, robj *obj) {
    size_t len = sdslen(obj->ptr);
    int enclen;

    /* Try integer encoding */
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding(obj->ptr,buf)) > 0) {
            if (fwrite(buf,enclen,1,fp) == 0) return -1;
            return 0;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (1 && len > 20) {
        int retval;

        retval = rdbSaveLzfStringObject(fp,obj);
        if (retval == -1) return -1;
        if (retval > 0) return 0;
        /* retval == 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    if (rdbSaveLen(fp,len) == -1) return -1;
    if (len && fwrite(obj->ptr,len,1,fp) == 0) return -1;
    return 0;
}

/* Save the DB on disk. Return REDIS_ERR on error, REDIS_OK on success */
static int rdbSave(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];//先写入到临时的rdb文件中
    int j;
    time_t now = time(NULL);

    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed saving the DB: %s", strerror(errno));
        return REDIS_ERR;
    }
    if (fwrite("REDIS0001",9,1,fp) == 0) goto werr;
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* Write the SELECT DB opcode */
        if (rdbSaveType(fp,REDIS_SELECTDB) == -1) goto werr;//数据类型保存到文件
        if (rdbSaveLen(fp,j) == -1) goto werr;//对不同大小的uint32_t进行保存

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            robj *key = dictGetEntryKey(de);//获取key
            robj *o = dictGetEntryVal(de);//获取value
            time_t expiretime = getExpire(db,key);//获得过期时间

            /* Save the expire time */
            if (expiretime != -1) {
                /* If this key is already expired skip it */
                if (expiretime < now) continue;//过期，跳过
                if (rdbSaveType(fp,REDIS_EXPIRETIME) == -1) goto werr;//保存类型
                if (rdbSaveTime(fp,expiretime) == -1) goto werr;//保存时间
            }
            /* Save the key and associated value */
            if (rdbSaveType(fp,o->type) == -1) goto werr;
            if (rdbSaveStringObject(fp,key) == -1) goto werr;//key是string类型的
            if (o->type == REDIS_STRING) {
                /* Save a string value */
                if (rdbSaveStringObject(fp,o) == -1) goto werr;
            } else if (o->type == REDIS_LIST) {
                /* Save a list value */
                list *list = o->ptr;
                listNode *ln;

                listRewind(list);//将链表中的迭代器变为正向迭代器
                if (rdbSaveLen(fp,listLength(list)) == -1) goto werr;//保存链表格式
                while((ln = listYield(list))) {
                    robj *eleobj = listNodeValue(ln);

                    if (rdbSaveStringObject(fp,eleobj) == -1) goto werr;//保存字符串
                }
            } else if (o->type == REDIS_SET) {//集合一个key对应多个value,key已经在上面保存过了
                /* Save a set value */
                dict *set = o->ptr;
                dictIterator *di = dictGetIterator(set);
                dictEntry *de;

                if (!set) oom("dictGetIteraotr");
                if (rdbSaveLen(fp,dictSize(set)) == -1) goto werr;
                while((de = dictNext(di)) != NULL) {
                    robj *eleobj = dictGetEntryKey(de);

                    if (rdbSaveStringObject(fp,eleobj) == -1) goto werr;
                }
                dictReleaseIterator(di);
            } else {
                assert(0 != 0);
            }
        }
        dictReleaseIterator(di);//释放迭代器
    }
    /* EOF opcode */
    if (rdbSaveType(fp,REDIS_EOF) == -1) goto werr;//写结束符号

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {//替换名字
        redisLog(REDIS_WARNING,"Error moving temp DB file on the final destionation: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

static int rdbSaveBackground(char *filename) {
    pid_t childpid;

    if (server.bgsaveinprogress) return REDIS_ERR;//保存的过程中不能保存
    if ((childpid = fork()) == 0) {    //子进程中fork的返回值是0
        /* Child */
        close(server.fd);
        if (rdbSave(filename) == REDIS_OK) {
            exit(0);
        } else {
            exit(1);
        }
    } else {
        /* Parent */
        if (childpid == -1) {  //fork失败，返回值为-1
            redisLog(REDIS_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,"Background saving started by pid %d",childpid);  //父进程中fork的返回值是子进程的PID
        server.bgsaveinprogress = 1;
        server.bgsavechildpid = childpid;//子进程
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}
//如果子进程在创建临时文件后崩溃或由于某种原因未能正常完成其任务，那么父进程（或任何其他负责清理的进程）可能会调用这个函数来尝试删除那个临时文件。
//然而，如果子进程在崩溃前已经将临时文件重命名为最终的 RDB 文件，那么调用 unlink 函数将不会删除任何文件，因为 unlink 只会删除指定路径名的文件，如果该文件不存在，则操作无效。
static void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}
//读取类型
static int rdbLoadType(FILE *fp) {
    unsigned char type;
    if (fread(&type,1,1,fp) == 0) return -1;
    return type;
}
//读取时间
static time_t rdbLoadTime(FILE *fp) {
    int32_t t32;
    if (fread(&t32,4,1,fp) == 0) return -1;
    return (time_t) t32;
}

/* Load an encoded length from the DB, see the REDIS_RDB_* defines on the top
 * of this file for a description of how this are stored on disk.
 *
 * isencoded is set to 1 if the readed length is not actually a length but
 * an "encoding type", check the above comments for more info */
 //如果
static uint32_t rdbLoadLen(FILE *fp, int rdbver, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;

    if (isencoded) *isencoded = 0;
    if (rdbver == 0) {
        if (fread(&len,4,1,fp) == 0) return REDIS_RDB_LENERR;
        return ntohl(len);
    } else {
        int type;

        if (fread(buf,1,1,fp) == 0) return REDIS_RDB_LENERR;
        type = (buf[0]&0xC0)>>6;//buf[0]&11000000
        if (type == REDIS_RDB_6BITLEN) {
            /* Read a 6 bit len */ //00开头
            return buf[0]&0x3F;
        } else if (type == REDIS_RDB_ENCVAL) {//11开头，此为而是“编码类型”type，这是因为，string被转化为int，现在要转化回去，并且string转化的int也分为6位，14位和32位，这个只是说明，下x的字节为string转化的int
            /* Read a 6 bit len encoding type */
            if (isencoded) *isencoded = 1;
            return buf[0]&0x3F;
        } else if (type == REDIS_RDB_14BITLEN) {
            /* Read a 14 bit len *///01开头
            if (fread(buf+1,1,1,fp) == 0) return REDIS_RDB_LENERR;
            return ((buf[0]&0x3F)<<8)|buf[1];
        } else {
            /* Read a 32 bit len *///10开头
            if (fread(&len,4,1,fp) == 0) return REDIS_RDB_LENERR;
            return ntohl(len);                      //网络序转主机序
        }
    }
}
//创建int类型变量
static robj *rdbLoadIntegerObject(FILE *fp, int enctype) {
    unsigned char enc[4];
    long long val;

    if (enctype == REDIS_RDB_ENC_INT8) {
        if (fread(enc,1,1,fp) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == REDIS_RDB_ENC_INT16) {
        uint16_t v;
        if (fread(enc,2,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == REDIS_RDB_ENC_INT32) {
        uint32_t v;
        if (fread(enc,4,1,fp) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        assert(0!=0);
    }
    return createObject(REDIS_STRING,sdscatprintf(sdsempty(),"%lld",val));
}
//创建字符串
static robj *rdbLoadLzfStringObject(FILE*fp, int rdbver) {
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    if ((clen = rdbLoadLen(fp,rdbver,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((len = rdbLoadLen(fp,rdbver,NULL)) == REDIS_RDB_LENERR) return NULL;
    if ((c = zmalloc(clen)) == NULL) goto err;
    if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    if (fread(c,clen,1,fp) == 0) goto err;
    if (lzf_decompress(c,clen,val,len) == 0) goto err;
    zfree(c);
    return createObject(REDIS_STRING,val);
err:
    zfree(c);
    sdsfree(val);
    return NULL;
}
//创建字符串robj，根据是否压缩然后判断保存为int还是string，如果没压缩则完全读取。
static robj *rdbLoadStringObject(FILE*fp, int rdbver) {
    int isencoded;
    uint32_t len;
    sds val;

    len = rdbLoadLen(fp,rdbver,&isencoded);//读取一个长度，如果isencoded=1说明下一个是string转化的int
    if (isencoded) {
        switch(len) {
        case REDIS_RDB_ENC_INT8://8 16 32都执行rdbLoadIntegerObject，因为没有break
        case REDIS_RDB_ENC_INT16:
        case REDIS_RDB_ENC_INT32:
            return tryObjectSharing(rdbLoadIntegerObject(fp,len));
        case REDIS_RDB_ENC_LZF:
            return tryObjectSharing(rdbLoadLzfStringObject(fp,rdbver));  //使用共享池哈希表，共享robj对象,基于引用计数的原理
        default:
            assert(0!=0);
        }
    }

    if (len == REDIS_RDB_LENERR) return NULL;
    val = sdsnewlen(NULL,len);
    if (len && fread(val,len,1,fp) == 0) {
        sdsfree(val);
        return NULL;
    }
    return tryObjectSharing(createObject(REDIS_STRING,val));
}

static int rdbLoad(char *filename) {
    FILE *fp;
    robj *keyobj = NULL;
    uint32_t dbid;
    int type, retval, rdbver;
    dict *d = server.db[0].dict;
    redisDb *db = server.db+0;
    char buf[1024];
    time_t expiretime = -1, now = time(NULL);

    fp = fopen(filename,"r");
    if (!fp) return REDIS_ERR;
    if (fread(buf,9,1,fp) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Wrong signature trying to load DB from file");
        return REDIS_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver > 1) {
        fclose(fp);
        redisLog(REDIS_WARNING,"Can't handle RDB format version %d",rdbver);
        return REDIS_ERR;
    }
    while(1) {
        robj *o;

        /* Read type. */
        if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        if (type == REDIS_EXPIRETIME) {
            if ((expiretime = rdbLoadTime(fp)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again */
            if ((type = rdbLoadType(fp)) == -1) goto eoferr;
        }
        if (type == REDIS_EOF) break;
        /* Handle SELECT DB opcode as a special case */
        if (type == REDIS_SELECTDB) {
            if ((dbid = rdbLoadLen(fp,rdbver,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                redisLog(REDIS_WARNING,"FATAL: Data file was created with a Redis server configured to handle more than %d databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            d = db->dict;
            continue;
        }
        /* Read key */
        if ((keyobj = rdbLoadStringObject(fp,rdbver)) == NULL) goto eoferr;

        if (type == REDIS_STRING) {
            /* Read string value */
            if ((o = rdbLoadStringObject(fp,rdbver)) == NULL) goto eoferr;
        } else if (type == REDIS_LIST || type == REDIS_SET) {
            /* Read list/set value */
            uint32_t listlen;

            if ((listlen = rdbLoadLen(fp,rdbver,NULL)) == REDIS_RDB_LENERR)
                goto eoferr;
            o = (type == REDIS_LIST) ? createListObject() : createSetObject();
            /* Load every single element of the list/set */
            while(listlen--) {
                robj *ele;

                if ((ele = rdbLoadStringObject(fp,rdbver)) == NULL) goto eoferr;
                if (type == REDIS_LIST) {
                    if (!listAddNodeTail((list*)o->ptr,ele))
                        oom("listAddNodeTail");
                } else {
                    if (dictAdd((dict*)o->ptr,ele,NULL) == DICT_ERR)
                        oom("dictAdd");
                }
            }
        } else {
            assert(0 != 0);
        }
        /* Add the new object in the hash table */
        retval = dictAdd(d,keyobj,o);
        if (retval == DICT_ERR) {
            redisLog(REDIS_WARNING,"Loading DB, duplicated key (%s) found! Unrecoverable error, exiting now.", keyobj->ptr);
            exit(1);
        }
        /* Set the expire time if needed */
        if (expiretime != -1) {
            setExpire(db,keyobj,expiretime);
            /* Delete this key if already expired */
            if (expiretime < now) deleteKey(db,keyobj);
            expiretime = -1;
        }
        keyobj = o = NULL;
    }
    fclose(fp);
    return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    if (keyobj) decrRefCount(keyobj);
    redisLog(REDIS_WARNING,"Short read or OOM loading DB. Unrecoverable error, exiting now.");
    exit(1);
    return REDIS_ERR; /* Just to avoid warning */
}

/*================================== Commands =============================== */

static void authCommand(redisClient *c) {
    if (!server.requirepass || !strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReply(c,shared.err);
    }
}
//ping pong
static void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}

static void echoCommand(redisClient *c) {
    addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",
        (int)sdslen(c->argv[1]->ptr)));
    addReply(c,c->argv[1]);
    addReply(c,shared.crlf);
}

/*=================================== Strings =============================== */

static void setGenericCommand(redisClient *c, int nx) {//nx为1,插入会失败
    int retval;

    retval = dictAdd(c->db->dict,c->argv[1],c->argv[2]);//向Hash表中增加键值
    if (retval == DICT_ERR) {
        if (!nx) {
            dictReplace(c->db->dict,c->argv[1],c->argv[2]);
            incrRefCount(c->argv[2]);
        } else {
            addReply(c,shared.czero);
            return;
        }
    } else {
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
    }
    server.dirty++;
    removeExpire(c->db,c->argv[1]);
    addReply(c, nx ? shared.cone : shared.ok);
}
//插入一个string 允许重复
static void setCommand(redisClient *c) {
    setGenericCommand(c,0);
}
//插入一个string 不允许重复
static void setnxCommand(redisClient *c) {
    setGenericCommand(c,1);
}

static void getCommand(redisClient *c) {
    robj *o = lookupKeyRead(c->db,c->argv[1]);

    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_STRING) {
            addReply(c,shared.wrongtypeerr);
        } else {
            addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",(int)sdslen(o->ptr)));//告诉接受长度
            addReply(c,o);
            addReply(c,shared.crlf);
        }
    }
}
//得到key保存的value，并更新
static void getSetCommand(redisClient *c) {
    getCommand(c);
    if (dictAdd(c->db->dict,c->argv[1],c->argv[2]) == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],c->argv[2]);
    } else {
        incrRefCount(c->argv[1]);
    }
    incrRefCount(c->argv[2]);
    server.dirty++;
    removeExpire(c->db,c->argv[1]);
}
//获取多个值
static void mgetCommand(redisClient *c) {
    int j;

    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",c->argc-1));
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != REDIS_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",(int)sdslen(o->ptr)));
                addReply(c,o);
                addReply(c,shared.crlf);
            }
        }
    }
}
//增加 incr
static void incrDecrCommand(redisClient *c, long long incr) {
    long long value;
    int retval;
    robj *o;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        value = 0;
    } else {
        if (o->type != REDIS_STRING) {
            value = 0;
        } else {
            char *eptr;

            value = strtoll(o->ptr, &eptr, 10);
        }
    }

    value += incr;
    o = createObject(REDIS_STRING,sdscatprintf(sdsempty(),"%lld",value));
    retval = dictAdd(c->db->dict,c->argv[1],o);
    if (retval == DICT_ERR) {
        dictReplace(c->db->dict,c->argv[1],o);
        removeExpire(c->db,c->argv[1]);
    } else {
        incrRefCount(c->argv[1]);
    }
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,o);
    addReply(c,shared.crlf);
}
//+1
static void incrCommand(redisClient *c) {
    incrDecrCommand(c,1);
}
//-1
static void decrCommand(redisClient *c) {
    incrDecrCommand(c,-1);
}
//增加 n
static void incrbyCommand(redisClient *c) {
    long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
    incrDecrCommand(c,incr);
}
// -n
static void decrbyCommand(redisClient *c) {
    long long incr = strtoll(c->argv[2]->ptr, NULL, 10);
    incrDecrCommand(c,-incr);
}

/* ========================= Type agnostic commands ========================= */
//删除
static void delCommand(redisClient *c) {
    int deleted = 0, j;

    for (j = 1; j < c->argc; j++) {
        if (deleteKey(c->db,c->argv[j])) {
            server.dirty++;
            deleted++;
        }
    }
    switch(deleted) {
    case 0:
        addReply(c,shared.czero);
        break;
    case 1:
        addReply(c,shared.cone);
        break;
    default:
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",deleted));
        break;
    }
}
//存在
static void existsCommand(redisClient *c) {
    addReply(c,lookupKeyRead(c->db,c->argv[1]) ? shared.cone : shared.czero);
}
//选择数据库
static void selectCommand(redisClient *c) {
    int id = atoi(c->argv[1]->ptr);

    if (selectDb(c,id) == REDIS_ERR) {
        addReplySds(c,sdsnew("-ERR invalid DB index\r\n"));
    } else {
        addReply(c,shared.ok);
    }
}
//返回随机件
static void randomkeyCommand(redisClient *c) {
    dictEntry *de;

    while(1) {
        de = dictGetRandomKey(c->db->dict);
        if (!de || expireIfNeeded(c->db,dictGetEntryKey(de)) == 0) break;
    }
    if (de == NULL) {
        addReply(c,shared.plus);
        addReply(c,shared.crlf);
    } else {
        addReply(c,shared.plus);
        addReply(c,dictGetEntryKey(de));
        addReply(c,shared.crlf);
    }
}
//查找符合正则表达式的key
static void keysCommand(redisClient *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;
    int plen = sdslen(pattern);
    int numkeys = 0, keyslen = 0;
    robj *lenobj = createObject(REDIS_STRING,NULL);

    di = dictGetIterator(c->db->dict);
    if (!di) oom("dictGetIterator");
    addReply(c,lenobj);
    decrRefCount(lenobj);
    while((de = dictNext(di)) != NULL) {
        robj *keyobj = dictGetEntryKey(de);

        sds key = keyobj->ptr;
        if ((pattern[0] == '*' && pattern[1] == '\0') ||
            stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            if (expireIfNeeded(c->db,keyobj) == 0) {
                if (numkeys != 0)
                    addReply(c,shared.space);
                addReply(c,keyobj);
                numkeys++;
                keyslen += sdslen(key);
            }
        }
    }
    dictReleaseIterator(di);
    lenobj->ptr = sdscatprintf(sdsempty(),"$%lu\r\n",keyslen+(numkeys ? (numkeys-1) : 0));
    addReply(c,shared.crlf);
}
//数据库大小
static void dbsizeCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",dictSize(c->db->dict)));
}
//上一次存储时间
static void lastsaveCommand(redisClient *c) {
    addReplySds(c,
        sdscatprintf(sdsempty(),":%lu\r\n",server.lastsave));
}
//查看key类型
static void typeCommand(redisClient *c) {
    robj *o;
    char *type;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        type = "+none";
    } else {
        switch(o->type) {
        case REDIS_STRING: type = "+string"; break;
        case REDIS_LIST: type = "+list"; break;
        case REDIS_SET: type = "+set"; break;
        default: type = "unknown"; break;
        }
    }
    addReplySds(c,sdsnew(type));
    addReply(c,shared.crlf);
}
//保存rdb
static void saveCommand(redisClient *c) {
    if (server.bgsaveinprogress) {
        addReplySds(c,sdsnew("-ERR background save in progress\r\n"));
        return;
    }
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
//新进程保存rdb
static void bgsaveCommand(redisClient *c) {
    if (server.bgsaveinprogress) {
        addReplySds(c,sdsnew("-ERR background save already in progress\r\n"));
        return;
    }
    if (rdbSaveBackground(server.dbfilename) == REDIS_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
//关闭服务器
static void shutdownCommand(redisClient *c) {
    redisLog(REDIS_WARNING,"User requested shutdown, saving DB...");
    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.bgsaveinprogress) {
        redisLog(REDIS_WARNING,"There is a live saving child. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    /* SYNC SAVE */
    if (rdbSave(server.dbfilename) == REDIS_OK) {
        if (server.daemonize)
            unlink(server.pidfile);
        redisLog(REDIS_WARNING,"%zu bytes used at exit",zmalloc_used_memory());
        redisLog(REDIS_WARNING,"Server exit now, bye bye...");
        exit(1);//退出
    } else {
        /* Ooops.. error saving! The best we can do is to continue operating.
         * Note that if there was a background saving process, in the next
         * cron() Redis will be notified that the background saving aborted,
         * handling special stuff like slaves pending for synchronization... */
        redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit");
        addReplySds(c,sdsnew("-ERR can't quit, problems saving the DB\r\n"));
    }
}
//重命名
static void renameGenericCommand(redisClient *c, int nx) {
    robj *o;

    /* To use the same key as src and dst is probably an error */
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nokeyerr);
        return;
    }
    incrRefCount(o);
    deleteIfVolatile(c->db,c->argv[2]);
    if (dictAdd(c->db->dict,c->argv[2],o) == DICT_ERR) {
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        dictReplace(c->db->dict,c->argv[2],o);
    } else {
        incrRefCount(c->argv[2]);
    }
    deleteKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,nx ? shared.cone : shared.ok);
}

static void renameCommand(redisClient *c) {
    renameGenericCommand(c,0);
}

static void renamenxCommand(redisClient *c) {
    renameGenericCommand(c,1);
}
//从当前库拿出一个key放到指定的库
static void moveCommand(redisClient *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;

    /* Obtain source and target DB pointers */
    src = c->db;
    srcid = c->db->id;
    if (selectDb(c,atoi(c->argv[2]->ptr)) == REDIS_ERR) {
        addReply(c,shared.outofrangeerr);
        return;
    }
    dst = c->db;
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);
        return;
    }

    /* Try to add the element to the target DB */
    deleteIfVolatile(dst,c->argv[1]);
    if (dictAdd(dst->dict,c->argv[1],o) == DICT_ERR) {
        addReply(c,shared.czero);
        return;
    }
    incrRefCount(c->argv[1]);
    incrRefCount(o);

    /* OK! key moved, free the entry in the source DB */
    deleteKey(src,c->argv[1]);
    server.dirty++;
    addReply(c,shared.cone);
}

/* =================================== Lists ================================ */
static void pushGenericCommand(redisClient *c, int where) {
    robj *lobj;
    list *list;

    lobj = lookupKeyWrite(c->db,c->argv[1]);
    if (lobj == NULL) {
        lobj = createListObject();
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            if (!listAddNodeHead(list,c->argv[2])) oom("listAddNodeHead");
        } else {
            if (!listAddNodeTail(list,c->argv[2])) oom("listAddNodeTail");
        }
        dictAdd(c->db->dict,c->argv[1],lobj);
        incrRefCount(c->argv[1]);
        incrRefCount(c->argv[2]);
    } else {
        if (lobj->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        list = lobj->ptr;
        if (where == REDIS_HEAD) {
            if (!listAddNodeHead(list,c->argv[2])) oom("listAddNodeHead");
        } else {
            if (!listAddNodeTail(list,c->argv[2])) oom("listAddNodeTail");
        }
        incrRefCount(c->argv[2]);
    }
    server.dirty++;
    addReply(c,shared.ok);
}
//lpush
static void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}
//rpush
static void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}
//list长度
static void llenCommand(redisClient *c) {
    robj *o;
    list *l;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
        return;
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            l = o->ptr;
            addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",listLength(l)));
        }
    }
}

static void lindexCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;

            ln = listIndex(list, index);//返回index位置的节点
            if (ln == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *ele = listNodeValue(ln);
                addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",(int)sdslen(ele->ptr)));
                addReply(c,ele);
                addReply(c,shared.crlf);
            }
        }
    }
}
//Redis 列表的指定索引位置设置一个新元素。
static void lsetCommand(redisClient *c) {
    robj *o;
    int index = atoi(c->argv[2]->ptr);

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nokeyerr);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;

            ln = listIndex(list, index);
            if (ln == NULL) {
                addReply(c,shared.outofrangeerr);
            } else {
                robj *ele = listNodeValue(ln);

                decrRefCount(ele);
                listNodeValue(ln) = c->argv[3];//获取链表当前节点的值(注意此处获取的指针)
                incrRefCount(c->argv[3]);
                addReply(c,shared.ok);
                server.dirty++;
            }
        }
    }
}

static void popGenericCommand(redisClient *c, int where) {
    robj *o;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;

            if (where == REDIS_HEAD)
                ln = listFirst(list);
            else
                ln = listLast(list);

            if (ln == NULL) {
                addReply(c,shared.nullbulk);
            } else {
                robj *ele = listNodeValue(ln);
                addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",(int)sdslen(ele->ptr)));
                addReply(c,ele);
                addReply(c,shared.crlf);
                listDelNode(list,ln);
                server.dirty++;
            }
        }
    }
}
//lpop
static void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}
//rpop
static void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}
//在list中获取下标left-right的值
static void lrangeCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nullmultibulk);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            int llen = listLength(list);
            int rangelen, j;
            robj *ele;

            /* convert negative indexes */
            if (start < 0) start = llen+start;
            if (end < 0) end = llen+end;
            if (start < 0) start = 0;
            if (end < 0) end = 0;

            /* indexes sanity checks */
            if (start > end || start >= llen) {
                /* Out of range start or start > end result in empty list */
                addReply(c,shared.emptymultibulk);
                return;
            }
            if (end >= llen) end = llen-1;
            rangelen = (end-start)+1;

            /* Return the result in form of a multi-bulk reply */
            ln = listIndex(list, start);
            addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",rangelen));
            for (j = 0; j < rangelen; j++) {
                ele = listNodeValue(ln);
                addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",(int)sdslen(ele->ptr)));
                addReply(c,ele);
                addReply(c,shared.crlf);
                ln = ln->next;
            }
        }
    }
}
//用法 ltrim list num1 num2 只保留list num1-num2的内容
static void ltrimCommand(redisClient *c) {
    robj *o;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.nokeyerr);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln;
            int llen = listLength(list);
            int j, ltrim, rtrim;

            /* convert negative indexes */
            if (start < 0) start = llen+start;
            if (end < 0) end = llen+end;
            if (start < 0) start = 0;
            if (end < 0) end = 0;

            /* indexes sanity checks */
            if (start > end || start >= llen) {
                /* Out of range start or start > end result in empty list */
                ltrim = llen;
                rtrim = 0;
            } else {
                if (end >= llen) end = llen-1;
                ltrim = start;
                rtrim = llen-end-1;
            }

            /* Remove list elements to perform the trim */
            for (j = 0; j < ltrim; j++) {
                ln = listFirst(list);
                listDelNode(list,ln);
            }
            for (j = 0; j < rtrim; j++) {
                ln = listLast(list);
                listDelNode(list,ln);
            }
            server.dirty++;
            addReply(c,shared.ok);
        }
    }
}
//LREM key count value 用于根据参数移除列表中等于特定值的元素。
//这个命令可以移除列表中所有等于某个值的元素，或者只移除前 N 个等于该值的元素。
//count是负数则从后向前。
static void lremCommand(redisClient *c) {
    robj *o;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
    } else {
        if (o->type != REDIS_LIST) {
            addReply(c,shared.wrongtypeerr);
        } else {
            list *list = o->ptr;
            listNode *ln, *next;
            int toremove = atoi(c->argv[2]->ptr);
            int removed = 0;
            int fromtail = 0;

            if (toremove < 0) {
                toremove = -toremove;
                fromtail = 1;
            }
            ln = fromtail ? list->tail : list->head;
            while (ln) {
                robj *ele = listNodeValue(ln);

                next = fromtail ? ln->prev : ln->next;
                if (sdscmp(ele->ptr,c->argv[3]->ptr) == 0) {
                    listDelNode(list,ln);
                    server.dirty++;
                    removed++;
                    if (toremove && removed == toremove) break;
                }
                ln = next;
            }
            addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",removed));
        }
    }
}

/* ==================================== Sets ================================ */
//set中增加一个key
static void saddCommand(redisClient *c) {
    robj *set;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        set = createSetObject();
        dictAdd(c->db->dict,c->argv[1],set);
        incrRefCount(c->argv[1]);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }
    if (dictAdd(set->ptr,c->argv[2],NULL) == DICT_OK) {
        incrRefCount(c->argv[2]);
        server.dirty++;
        addReply(c,shared.cone);
    } else {
        addReply(c,shared.czero);
    }
}
//删除key
static void sremCommand(redisClient *c) {
    robj *set;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.czero);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (dictDelete(set->ptr,c->argv[2]) == DICT_OK) {
            server.dirty++;
            if (htNeedsResize(set->ptr)) dictResize(set->ptr);
            addReply(c,shared.cone);
        } else {
            addReply(c,shared.czero);
        }
    }
}
//将set a中元素x移到set b
static void smoveCommand(redisClient *c) {
    robj *srcset, *dstset;

    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);

    /* If the source key does not exist return 0, if it's of the wrong type
     * raise an error */
    if (srcset == NULL || srcset->type != REDIS_SET) {
        addReply(c, srcset ? shared.wrongtypeerr : shared.czero);
        return;
    }
    /* Error if the destination key is not a set as well */
    if (dstset && dstset->type != REDIS_SET) {
        addReply(c,shared.wrongtypeerr);
        return;
    }
    /* Remove the element from the source set */
    if (dictDelete(srcset->ptr,c->argv[3]) == DICT_ERR) {
        /* Key not found in the src set! return zero */
        addReply(c,shared.czero);
        return;
    }
    server.dirty++;
    /* Add the element to the destination set */
    if (!dstset) {
        dstset = createSetObject();
        dictAdd(c->db->dict,c->argv[2],dstset);
        incrRefCount(c->argv[2]);
    }
    if (dictAdd(dstset->ptr,c->argv[3],NULL) == DICT_OK)
        incrRefCount(c->argv[3]);
    addReply(c,shared.cone);
}
//查看set a中是不是有元素x
static void sismemberCommand(redisClient *c) {
    robj *set;

    set = lookupKeyRead(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.czero);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        if (dictFind(set->ptr,c->argv[2]))
            addReply(c,shared.cone);
        else
            addReply(c,shared.czero);
    }
}
//求s的大小
static void scardCommand(redisClient *c) {
    robj *o;
    dict *s;

    o = lookupKeyRead(c->db,c->argv[1]);
    if (o == NULL) {
        addReply(c,shared.czero);
        return;
    } else {
        if (o->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
        } else {
            s = o->ptr;
            addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",
                dictSize(s)));
        }
    }
}
//SPOP命令用于从集合中随机移除一个元素
static void spopCommand(redisClient *c) {
    robj *set;
    dictEntry *de;

    set = lookupKeyWrite(c->db,c->argv[1]);
    if (set == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        if (set->type != REDIS_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
        de = dictGetRandomKey(set->ptr);
        if (de == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            robj *ele = dictGetEntryKey(de);

            addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",sdslen(ele->ptr)));
            addReply(c,ele);
            addReply(c,shared.crlf);
            dictDelete(set->ptr,ele);
            if (htNeedsResize(set->ptr)) dictResize(set->ptr);
            server.dirty++;
        }
    }
}
//比较函数
static int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    dict **d1 = (void*) s1, **d2 = (void*) s2;

    return dictSize(*d1)-dictSize(*d2);
}
//这段代码实现了 Redis 中处理集合交集（SINTER）命令的通用逻辑，
//它既可以用于计算多个集合的交集并将结果返回给客户端，也可以将结果存储到指定的键中。
//该命令可以求多个set的交集
static void sinterGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);//将多个集合储存在这个里面
    dictIterator *di;
    dictEntry *de;
    robj *lenobj = NULL, *dstset = NULL;
    int j, cardinality = 0;

    if (!dv) oom("sinterGenericCommand");
    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            zfree(dv);
            if (dstkey) {
                deleteKey(c->db,dstkey);
                addReply(c,shared.ok);
            } else {
                addReply(c,shared.nullmultibulk);
            }
            return;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    qsort(dv,setsnum,sizeof(dict*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        lenobj = createObject(REDIS_STRING,NULL);
        addReply(c,lenobj);
        decrRefCount(lenobj);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createSetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    di = dictGetIterator(dv[0]);
    if (!di) oom("dictGetIterator");

    while((de = dictNext(di)) != NULL) {
        robj *ele;

        for (j = 1; j < setsnum; j++)
            if (dictFind(dv[j],dictGetEntryKey(de)) == NULL) break;
        if (j != setsnum)
            continue; /* at least one set does not contain the member */
        ele = dictGetEntryKey(de);
        if (!dstkey) {
            addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",sdslen(ele->ptr)));
            addReply(c,ele);
            addReply(c,shared.crlf);
            cardinality++;
        } else {
            dictAdd(dstset->ptr,ele,NULL);
            incrRefCount(ele);
        }
    }
    dictReleaseIterator(di);

    if (dstkey) {
        /* Store the resulting set into the target */
        deleteKey(c->db,dstkey);
        dictAdd(c->db->dict,dstkey,dstset);
        incrRefCount(dstkey);
    }

    if (!dstkey) {
        lenobj->ptr = sdscatprintf(sdsempty(),"*%d\r\n",cardinality);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",
            dictSize((dict*)dstset->ptr)));
        server.dirty++;
    }
    zfree(dv);
}

static void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

static void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1

static void sunionDiffGenericCommand(redisClient *c, robj **setskeys, int setsnum, robj *dstkey, int op) {
    dict **dv = zmalloc(sizeof(dict*)*setsnum);
    dictIterator *di;
    dictEntry *de;
    robj *dstset = NULL;
    int j, cardinality = 0;

    if (!dv) oom("sunionDiffGenericCommand");
    for (j = 0; j < setsnum; j++) {
        robj *setobj;

        setobj = dstkey ?
                    lookupKeyWrite(c->db,setskeys[j]) :
                    lookupKeyRead(c->db,setskeys[j]);
        if (!setobj) {
            dv[j] = NULL;
            continue;
        }
        if (setobj->type != REDIS_SET) {
            zfree(dv);
            addReply(c,shared.wrongtypeerr);
            return;
        }
        dv[j] = setobj->ptr;
    }

    /* We need a temp set object to store our union. If the dstkey
     * is not NULL (that is, we are inside an SUNIONSTORE operation) then
     * this set object will be the resulting object to set into the target key*/
    dstset = createSetObject();

    /* Iterate all the elements of all the sets, add every element a single
     * time to the result set */
    for (j = 0; j < setsnum; j++) {
        if (op == REDIS_OP_DIFF && j == 0 && !dv[j]) break; /* result set is empty */
        if (!dv[j]) continue; /* non existing keys are like empty sets */

        di = dictGetIterator(dv[j]);
        if (!di) oom("dictGetIterator");

        while((de = dictNext(di)) != NULL) {
            robj *ele;

            /* dictAdd will not add the same element multiple times */
            ele = dictGetEntryKey(de);
            if (op == REDIS_OP_UNION || j == 0) {
                if (dictAdd(dstset->ptr,ele,NULL) == DICT_OK) {
                    incrRefCount(ele);
                    cardinality++;
                }
            } else if (op == REDIS_OP_DIFF) {
                if (dictDelete(dstset->ptr,ele) == DICT_OK) {
                    cardinality--;
                }
            }
        }
        dictReleaseIterator(di);

        if (op == REDIS_OP_DIFF && cardinality == 0) break; /* result set is empty */
    }

    /* Output the content of the resulting set, if not in STORE mode */
    if (!dstkey) {
        addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",cardinality));
        di = dictGetIterator(dstset->ptr);
        if (!di) oom("dictGetIterator");
        while((de = dictNext(di)) != NULL) {
            robj *ele;

            ele = dictGetEntryKey(de);
            addReplySds(c,sdscatprintf(sdsempty(),
                    "$%d\r\n",sdslen(ele->ptr)));
            addReply(c,ele);
            addReply(c,shared.crlf);
        }
        dictReleaseIterator(di);
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        deleteKey(c->db,dstkey);
        dictAdd(c->db->dict,dstkey,dstset);
        incrRefCount(dstkey);
    }

    /* Cleanup */
    if (!dstkey) {
        decrRefCount(dstset);
    } else {
        addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",
            dictSize((dict*)dstset->ptr)));
        server.dirty++;
    }
    zfree(dv);
}

static void sunionCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_UNION);
}

static void sunionstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_UNION);
}

static void sdiffCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,REDIS_OP_DIFF);
}

static void sdiffstoreCommand(redisClient *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],REDIS_OP_DIFF);
}
//清空当前数据库
static void flushdbCommand(redisClient *c) {
    server.dirty += dictSize(c->db->dict);
    dictEmpty(c->db->dict);
    dictEmpty(c->db->expires);
    addReply(c,shared.ok);
}
//清空所有数据库
static void flushallCommand(redisClient *c) {
    server.dirty += emptyDb();
    addReply(c,shared.ok);
    rdbSave(server.dbfilename);
    server.dirty++;
}

static redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    if (!so) oom("createSortOperation");
    so->type = type;
    so->pattern = pattern;
    return so;
}

/* Return the value associated to the key with a name obtained
 * substituting the first occurence of '*' in 'pattern' with 'subst' */
static robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p;
    sds spat, ssub;
    robj keyobj;
    int prefixlen, sublen, postfixlen;
    /* Expoit the internal sds representation to create a sds string allocated on the stack in order to make this function faster */
    struct {
        long len;
        long free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname;

    spat = pattern->ptr;
    ssub = subst->ptr;
    if (sdslen(spat)+sdslen(ssub)-1 > REDIS_SORTKEY_MAX) return NULL;
    p = strchr(spat,'*');
    if (!p) return NULL;

    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1);
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;

    keyobj.refcount = 1;
    keyobj.type = REDIS_STRING;
    keyobj.ptr = ((char*)&keyname)+(sizeof(long)*2);

    /* printf("lookup '%s' => %p\n", keyname.buf,de); */
    return lookupKeyRead(db,&keyobj);
}

/* sortCompare() is used by qsort in sortCommand(). Given that qsort_r with
 * the additional parameter is not standard but a BSD-specific we have to
 * pass sorting parameters via the global 'server' structure */
 //这段代码是一个用于Redis排序功能的比较函数，它根据Redis服务器的配置（如是否按字母顺序排序、是否按模式排序等）
 //来比较两个元素（redisSortObject 结构体），并返回一个整数来表示这两个元素之间的顺序关系。
static int sortCompare(const void *s1, const void *s2) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;

    if (!server.sort_alpha) {//不按照字母排序
        /* Numeric sorting. Here it's trivial as we precomputed scores */
        if (so1->u.score > so2->u.score) {
            cmp = 1;
        } else if (so1->u.score < so2->u.score) {
            cmp = -1;
        } else {
            cmp = 0;
        }
    } else {
        /* Alphanumeric sorting */
        if (server.sort_bypattern) {//按照模式排序
            if (!so1->u.cmpobj || !so2->u.cmpobj) {
                /* At least one compare object is NULL */
                if (so1->u.cmpobj == so2->u.cmpobj)
                    cmp = 0;
                else if (so1->u.cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                /* We have both the objects, use strcoll */
                cmp = strcoll(so1->u.cmpobj->ptr,so2->u.cmpobj->ptr);//用来排序
            }
        } else {
            /* Compare elements directly */
            cmp = strcoll(so1->obj->ptr,so2->obj->ptr);
        }
    }
    return server.sort_desc ? -cmp : cmp;
}

/* The SORT command is the most complex command in Redis. Warning: this code
 * is optimized for speed and a bit less for readability */
static void sortCommand(redisClient *c) {
    list *operations;
    int outputlen = 0;
    int desc = 0, alpha = 0;
    int limit_start = 0, limit_count = -1, start, end;
    int j, dontsort = 0, vectorlen;
    int getop = 0; /* GET operation counter */
    robj *sortval, *sortby = NULL;
    redisSortObject *vector; /* Resulting vector to sort */

    /* Lookup the key to sort. It must be of the right types */
    sortval = lookupKeyRead(c->db,c->argv[1]);
    if (sortval == NULL) {
        addReply(c,shared.nokeyerr);
        return;
    }
    if (sortval->type != REDIS_SET && sortval->type != REDIS_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    /* Create a list of operations to perform for every sorted element.
     * Operations can be GET/DEL/INCR/DECR */
    operations = listCreate();
    listSetFreeMethod(operations,zfree);
    j = 2;

    /* Now we need to protect sortval incrementing its count, in the future
     * SORT may have options able to overwrite/delete keys during the sorting
     * and the sorted key itself may get destroied */
    incrRefCount(sortval);

    /* The SORT command has an SQL-alike syntax, parse it */
    while(j < c->argc) {
        int leftargs = c->argc-j-1;
        if (!strcasecmp(c->argv[j]->ptr,"asc")) {
            desc = 0;
        } else if (!strcasecmp(c->argv[j]->ptr,"desc")) {
            desc = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"alpha")) {
            alpha = 1;
        } else if (!strcasecmp(c->argv[j]->ptr,"limit") && leftargs >= 2) {
            limit_start = atoi(c->argv[j+1]->ptr);
            limit_count = atoi(c->argv[j+2]->ptr);
            j+=2;
        } else if (!strcasecmp(c->argv[j]->ptr,"by") && leftargs >= 1) {
            sortby = c->argv[j+1];
            /* If the BY pattern does not contain '*', i.e. it is constant,
             * we don't need to sort nor to lookup the weight keys. */
            if (strchr(c->argv[j+1]->ptr,'*') == NULL) dontsort = 1;//strchr 函数是 C 语言标准库中的一个函数，用于在一个字符串中查找第一次出现的指定字符。
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_GET,c->argv[j+1]));
            getop++;
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"del") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_DEL,c->argv[j+1]));
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"incr") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_INCR,c->argv[j+1]));
            j++;
        } else if (!strcasecmp(c->argv[j]->ptr,"get") && leftargs >= 1) {
            listAddNodeTail(operations,createSortOperation(
                REDIS_SORT_DECR,c->argv[j+1]));
            j++;
        } else {
            decrRefCount(sortval);
            listRelease(operations);
            addReply(c,shared.syntaxerr);
            return;
        }
        j++;
    }

    /* Load the sorting vector with all the objects to sort */
    vectorlen = (sortval->type == REDIS_LIST) ?
        listLength((list*)sortval->ptr) :
        dictSize((dict*)sortval->ptr);
    vector = zmalloc(sizeof(redisSortObject)*vectorlen);
    if (!vector) oom("allocating objects vector for SORT");
    j = 0;
    if (sortval->type == REDIS_LIST) {
        list *list = sortval->ptr;
        listNode *ln;

        listRewind(list);
        while((ln = listYield(list))) {
            robj *ele = ln->value;
            vector[j].obj = ele;
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
    } else {
        dict *set = sortval->ptr;
        dictIterator *di;
        dictEntry *setele;

        di = dictGetIterator(set);
        if (!di) oom("dictGetIterator");
        while((setele = dictNext(di)) != NULL) {
            vector[j].obj = dictGetEntryKey(setele);
            vector[j].u.score = 0;
            vector[j].u.cmpobj = NULL;
            j++;
        }
        dictReleaseIterator(di);
    }
    assert(j == vectorlen);

    /* Now it's time to load the right scores in the sorting vector */
    if (dontsort == 0) {
        for (j = 0; j < vectorlen; j++) {
            if (sortby) {
                robj *byval;

                byval = lookupKeyByPattern(c->db,sortby,vector[j].obj);
                if (!byval || byval->type != REDIS_STRING) continue;
                if (alpha) {
                    vector[j].u.cmpobj = byval;
                    incrRefCount(byval);
                } else {
                    vector[j].u.score = strtod(byval->ptr,NULL);
                }
            } else {
                if (!alpha) vector[j].u.score = strtod(vector[j].obj->ptr,NULL);
            }
        }
    }

    /* We are ready to sort the vector... perform a bit of sanity check
     * on the LIMIT option too. We'll use a partial version of quicksort. */
    start = (limit_start < 0) ? 0 : limit_start;
    end = (limit_count < 0) ? vectorlen-1 : start+limit_count-1;
    if (start >= vectorlen) {
        start = vectorlen-1;
        end = vectorlen-2;
    }
    if (end >= vectorlen) end = vectorlen-1;

    if (dontsort == 0) {
        server.sort_desc = desc;
        server.sort_alpha = alpha;
        server.sort_bypattern = sortby ? 1 : 0;
        if (sortby && (start != 0 || end != vectorlen-1))
            pqsort(vector,vectorlen,sizeof(redisSortObject),sortCompare, start,end);
        else
            qsort(vector,vectorlen,sizeof(redisSortObject),sortCompare);
    }

    /* Send command output to the output buffer, performing the specified
     * GET/DEL/INCR/DECR operations if any. */
    outputlen = getop ? getop*(end-start+1) : end-start+1;
    addReplySds(c,sdscatprintf(sdsempty(),"*%d\r\n",outputlen));
    for (j = start; j <= end; j++) {
        listNode *ln;
        if (!getop) {
            addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",
                sdslen(vector[j].obj->ptr)));
            addReply(c,vector[j].obj);
            addReply(c,shared.crlf);
        }
        listRewind(operations);
        while((ln = listYield(operations))) {
            redisSortOperation *sop = ln->value;
            robj *val = lookupKeyByPattern(c->db,sop->pattern,
                vector[j].obj);

            if (sop->type == REDIS_SORT_GET) {
                if (!val || val->type != REDIS_STRING) {
                    addReply(c,shared.nullbulk);
                } else {
                    addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",
                        sdslen(val->ptr)));
                    addReply(c,val);
                    addReply(c,shared.crlf);
                }
            } else if (sop->type == REDIS_SORT_DEL) {
                /* TODO */
            }
        }
    }

    /* Cleanup */
    decrRefCount(sortval);
    listRelease(operations);
    for (j = 0; j < vectorlen; j++) {
        if (sortby && alpha && vector[j].u.cmpobj)
            decrRefCount(vector[j].u.cmpobj);
    }
    zfree(vector);
}
//打印一些日志
static void infoCommand(redisClient *c) {
    sds info;
    time_t uptime = time(NULL)-server.stat_starttime;
    int j;

    info = sdscatprintf(sdsempty(),
        "redis_version:%s\r\n"
        "uptime_in_seconds:%d\r\n"
        "uptime_in_days:%d\r\n"
        "connected_clients:%d\r\n"
        "connected_slaves:%d\r\n"
        "used_memory:%zu\r\n"
        "changes_since_last_save:%lld\r\n"
        "bgsave_in_progress:%d\r\n"
        "last_save_time:%d\r\n"
        "total_connections_received:%lld\r\n"
        "total_commands_processed:%lld\r\n"
        "role:%s\r\n"
        ,REDIS_VERSION,
        uptime,
        uptime/(3600*24),
        listLength(server.clients)-listLength(server.slaves),
        listLength(server.slaves),
        server.usedmemory,
        server.dirty,
        server.bgsaveinprogress,
        server.lastsave,
        server.stat_numconnections,
        server.stat_numcommands,
        server.masterhost == NULL ? "master" : "slave"
    );
    if (server.masterhost) {
        info = sdscatprintf(info,
            "master_host:%s\r\n"
            "master_port:%d\r\n"
            "master_link_status:%s\r\n"
            "master_last_io_seconds_ago:%d\r\n"
            ,server.masterhost,
            server.masterport,
            (server.replstate == REDIS_REPL_CONNECTED) ?
                "up" : "down",
            (int)(time(NULL)-server.master->lastinteraction)
        );
    }
    for (j = 0; j < server.dbnum; j++) {
        long long keys, vkeys;

        keys = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (keys || vkeys) {
            info = sdscatprintf(info, "db%d: keys=%lld,expires=%lld\r\n",
                j, keys, vkeys);
        }
    }
    addReplySds(c,sdscatprintf(sdsempty(),"$%d\r\n",sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

static void monitorCommand(redisClient *c) {
    /* ignore MONITOR if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    if (!listAddNodeTail(server.monitors,c)) oom("listAddNodeTail");
    addReply(c,shared.ok);
}

/* ================================= Expire ================================= */
//删除expire
static int removeExpire(redisDb *db, robj *key) {
    if (dictDelete(db->expires,key) == DICT_OK) {
        return 1;
    } else {
        return 0;
    }
}
//设置expire
static int setExpire(redisDb *db, robj *key, time_t when) {
    if (dictAdd(db->expires,key,(void*)when) == DICT_ERR) {
        return 0;
    } else {
        incrRefCount(key);
        return 1;
    }
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
 //获得过期时间
static time_t getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return -1;

    return (time_t) dictGetEntryVal(de);
}

static int expireIfNeeded(redisDb *db, robj *key) {
    time_t when;
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;

    /* Lookup the expire */
    when = (time_t) dictGetEntryVal(de);
    if (time(NULL) <= when) return 0;

    /* Delete the key */
    dictDelete(db->expires,key);
    return dictDelete(db->dict,key) == DICT_OK;
}
//deleteIfVolatile 函数是 Redis 中的一个函数，用于检查并删除具有过期时间的键
//（即“易失”键）。如果键存在且已过期，则该函数会将其从数据库中删除。
static int deleteIfVolatile(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key)) == NULL) return 0;

    /* Delete the key */
    server.dirty++;
    dictDelete(db->expires,key);
    return dictDelete(db->dict,key) == DICT_OK;
}
//增加过期时间
static void expireCommand(redisClient *c) {
    dictEntry *de;
    int seconds = atoi(c->argv[2]->ptr);

    de = dictFind(c->db->dict,c->argv[1]);
    if (de == NULL) {
        addReply(c,shared.czero);
        return;
    }
    if (seconds <= 0) {
        addReply(c, shared.czero);
        return;
    } else {
        time_t when = time(NULL)+seconds;
        if (setExpire(c->db,c->argv[1],when)) {
            addReply(c,shared.cone);
            server.dirty++;
        } else {
            addReply(c,shared.czero);
        }
        return;
    }
}
//返回过期时间
static void ttlCommand(redisClient *c) {
    time_t expire;
    int ttl = -1;

    expire = getExpire(c->db,c->argv[1]);
    if (expire != -1) {
        ttl = (int) (expire-time(NULL));
        if (ttl < 0) ttl = -1;
    }
    addReplySds(c,sdscatprintf(sdsempty(),":%d\r\n",ttl));
}

/* =============================== Replication  ============================= */
//在规定时间内读
static int syncWrite(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nwritten, ret = size;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_WRITABLE,1000) & AE_WRITABLE) {
            nwritten = write(fd,ptr,size);
            if (nwritten == -1) return -1;
            ptr += nwritten;
            size -= nwritten;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return ret;
}
//在规定时间内写
static int syncRead(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread, totread = 0;
    time_t start = time(NULL);

    timeout++;
    while(size) {
        if (aeWait(fd,AE_READABLE,1000) & AE_READABLE) {
            nread = read(fd,ptr,size);
            if (nread == -1) return -1;
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if ((time(NULL)-start) > timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
    return totread;
}
//读取一行
static int syncReadLine(int fd, char *ptr, ssize_t size, int timeout) {
    ssize_t nread = 0;

    size--;
    while(size) {
        char c;

        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        if (c == '\n') {
            *ptr = '\0';
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
    }
    return nread;
}
//进行主从同步
static void syncCommand(redisClient *c) {
    /* ignore SYNC if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    //检查发送列表是否还有数据没发送
    if (listLength(c->reply) != 0) {
        addReplySds(c,sdsnew("-ERR SYNC is invalid with pending input\r\n"));
        return;
    }
    //从属设备请求同步
    redisLog(REDIS_NOTICE,"Slave ask for synchronization");
    /* Here we need to check if there is a background saving operation
     * in progress, or if it is required to start one */
    if (server.bgsaveinprogress) {//如果已经有一个后台保存操作在进行中，函数会检查是否有其他从服务器已经在等待这个 BGSAVE 操作的结束以获取差异数据。
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save */
        redisClient *slave;
        listNode *ln;

        listRewind(server.slaves);
        while((ln = listYield(server.slaves))) {//函数会检查是否有其他从服务器已经在等待这个 BGSAVE 操作的结束以获取差异数据。
            slave = ln->value;
            if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) break;
        }
        if (ln) {//如果有，那么当前从服务器将共享这个差异数据列表
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            listRelease(c->reply);//释放链表
            c->reply = listDup(slave->reply);//复制从表的回复链表
            if (!c->reply) oom("listDup copying slave reply list");
            c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
            redisLog(REDIS_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {//如果没有其他从服务器在等待，那么当前从服务器的 replstate 将被设置为 REDIS_REPL_WAIT_BGSAVE_START，表示它将等待下一个 BGSAVE 操作开始。
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences */
            c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
            redisLog(REDIS_NOTICE,"Waiting for next BGSAVE for SYNC");
        }
    } else {//如果没有后台保存操作正在进行，则启动一个新的 BGSAVE 操作，并将从服务器的 replstate 设置为 REDIS_REPL_WAIT_BGSAVE_END。
        /* Ok we don't have a BGSAVE in progress, let's start one */
        redisLog(REDIS_NOTICE,"Starting BGSAVE for SYNC");
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            redisLog(REDIS_NOTICE,"Replication failed, can't BGSAVE");
            addReplySds(c,sdsnew("-ERR Unalbe to perform background save\r\n"));
            return;
        }
        c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
    }
    c->repldbfd = -1;//复制数据库文件描述符
    c->flags |= REDIS_SLAVE;
    c->slaveseldb = 0;//如果这个客户端是一个从服务器，这个字段存储了它选择的数据库 ID。这个值在主从复制过程中用于同步数据。
    if (!listAddNodeTail(server.slaves,c)) oom("listAddNodeTail");//加入从服务器列表
    return;
}

static void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *slave = privdata;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);
    char buf[REDIS_IOBUF_LEN];
    ssize_t nwritten, buflen;
    //第一次同步
    if (slave->repldboff == 0) {//复制数据库文件的当前偏移量。在从服务器读取主服务器发送的数据时，这个值会不断更新，以指示已经读取了多少数据。
        /* Write the bulk write count before to transfer the DB. In theory here
         * we don't know how much room there is in the output buffer of the
         * socket, but in pratice SO_SNDLOWAT (the minimum count for output
         * operations) will never be smaller than the few bytes we need. */
        sds bulkcount;

        bulkcount = sdscatprintf(sdsempty(),"$%lld\r\n",(unsigned long long)
            slave->repldbsize);
        if (write(fd,bulkcount,sdslen(bulkcount)) != (signed)sdslen(bulkcount))
        {
            sdsfree(bulkcount);
            freeClient(slave);
            return;
        }
        sdsfree(bulkcount);
    }
    //从slave->repldbfd读取偏移量slave->repldboff的数据
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    buflen = read(slave->repldbfd,buf,REDIS_IOBUF_LEN);
    if (buflen <= 0) {
        redisLog(REDIS_WARNING,"Read error sending DB to slave: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    //发送数据
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        redisLog(REDIS_DEBUG,"Write error sending DB to slave: %s",
            strerror(errno));
        freeClient(slave);
        return;
    }
    //偏移量增加
    slave->repldboff += nwritten;
    if (slave->repldboff == slave->repldbsize) {
        close(slave->repldbfd);
        slave->repldbfd = -1;
        //调用该函数之前，已经注册进select了，所以现在去掉。
        aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
        slave->replstate = REDIS_REPL_ONLINE;
        if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE,
            sendReplyToClient, slave) == AE_ERR) {
            freeClient(slave);
            return;
        }
        addReplySds(slave,sdsempty());
        redisLog(REDIS_NOTICE,"Synchronization with slave succeeded");
    }
}

/* This function is called at the end of every backgrond saving.
 * The argument bgsaveerr is REDIS_OK if the background saving succeeded
 * otherwise REDIS_ERR is passed to the function.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization. */
 //在定时任务中，bgsave结束后会调用此函数，说明rdb快照保存成功，现在注册可写事件，把数据库发送出去
static void updateSlavesWaitingBgsave(int bgsaveerr) {
    listNode *ln;
    int startbgsave = 0;

    listRewind(server.slaves);
    while((ln = listYield(server.slaves))) {
        redisClient *slave = ln->value;

        if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
            startbgsave = 1;
            slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
        } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            if (bgsaveerr != REDIS_OK) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }
            if ((slave->repldbfd = open(server.dbfilename,O_RDONLY)) == -1 ||
                redis_fstat(slave->repldbfd,&buf) == -1) {
                freeClient(slave);
                redisLog(REDIS_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                continue;
            }
            slave->repldboff = 0;
            slave->repldbsize = buf.st_size;
            slave->replstate = REDIS_REPL_SEND_BULK;
            aeDeleteFileEvent(server.el,slave->fd,AE_WRITABLE);
            if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                freeClient(slave);
                continue;
            }
        }
    }
    if (startbgsave) {//REDIS_REPL_WAIT_BGSAVE_START要等待下一次rdb保存，现在保存
    //如果rdb保存失败，则关闭所有REDIS_REPL_WAIT_BGSAVE_START状态的从服务器
        if (rdbSaveBackground(server.dbfilename) != REDIS_OK) {
            listRewind(server.slaves);
            redisLog(REDIS_WARNING,"SYNC failed. BGSAVE failed");
            while((ln = listYield(server.slaves))) {
                redisClient *slave = ln->value;

                if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
            }
        }
    }
}
//从主服务器接收数据库的快照（dump）文件，并将其加载到当前Redis服务器的数据库中，以完成与主服务器的数据同步。
static int syncWithMaster(void) {
    char buf[1024], tmpfile[256];
    int dumpsize;
    int fd = anetTcpConnect(NULL,server.masterhost,server.masterport);
    int dfd;

    if (fd == -1) {
        redisLog(REDIS_WARNING,"Unable to connect to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    /* Issue the SYNC command */
    //发送SYNC
    if (syncWrite(fd,"SYNC \r\n",7,5) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error writing to MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    /* Read the bulk write count */
    //读取长度
    if (syncReadLine(fd,buf,1024,3600) == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"I/O error reading bulk count from MASTER: %s",
            strerror(errno));
        return REDIS_ERR;
    }
    dumpsize = atoi(buf+1);
    redisLog(REDIS_NOTICE,"Receiving %d bytes data dump from MASTER",dumpsize);
    /* Read the bulk write data on a temp file */
    snprintf(tmpfile,256,"temp-%d.%ld.rdb",(int)time(NULL),(long int)random());
    dfd = open(tmpfile,O_CREAT|O_WRONLY,0644);
    if (dfd == -1) {
        close(fd);
        redisLog(REDIS_WARNING,"Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        return REDIS_ERR;
    }
    while(dumpsize) {
        int nread, nwritten;

        nread = read(fd,buf,(dumpsize < 1024)?dumpsize:1024);
        if (nread == -1) {
            redisLog(REDIS_WARNING,"I/O error trying to sync with MASTER: %s",
                strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        nwritten = write(dfd,buf,nread);
        if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Write error writing to the DB dump file needed for MASTER <-> SLAVE synchrnonization: %s", strerror(errno));
            close(fd);
            close(dfd);
            return REDIS_ERR;
        }
        dumpsize -= nread;
    }
    close(dfd);
    if (rename(tmpfile,server.dbfilename) == -1) {
        redisLog(REDIS_WARNING,"Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s", strerror(errno));
        unlink(tmpfile);
        close(fd);
        return REDIS_ERR;
    }
    //清空数据库
    emptyDb();
    //读取rdb
    if (rdbLoad(server.dbfilename) != REDIS_OK) {
        redisLog(REDIS_WARNING,"Failed trying to load the MASTER synchronization DB from disk");
        close(fd);
        return REDIS_ERR;
    }
    server.master = createClient(fd);
    server.master->flags |= REDIS_MASTER;
    server.replstate = REDIS_REPL_CONNECTED;
    return REDIS_OK;
}
//成为一个服务器的从服务器
static void slaveofCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            sdsfree(server.masterhost);
            server.masterhost = NULL;
            if (server.master) freeClient(server.master);
            server.replstate = REDIS_REPL_NONE;
            redisLog(REDIS_NOTICE,"MASTER MODE enabled (user request)");
        }
    } else {
        sdsfree(server.masterhost);
        server.masterhost = sdsdup(c->argv[1]->ptr);
        server.masterport = atoi(c->argv[2]->ptr);
        if (server.master) freeClient(server.master);
        server.replstate = REDIS_REPL_CONNECT;
        redisLog(REDIS_NOTICE,"SLAVE OF %s:%d enabled (user request)",
            server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* This function gets called when 'maxmemory' is set on the config file to limit
 * the max memory used by the server, and we are out of memory.
 * This function will try to, in order:
 *
 * - Free objects from the free list
 * - Try to remove keys with an EXPIRE set
 *
 * It is not possible to free enough memory to reach used-memory < maxmemory
 * the server will start refusing commands that will enlarge even more the
 * memory usage.
 *///释放内存，如果超过了最大限制
static void freeMemoryIfNeeded(void) {
    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        if (listLength(server.objfreelist)) {//从objfreelist中删除
            robj *o;

            listNode *head = listFirst(server.objfreelist);
            o = listNodeValue(head);
            listDelNode(server.objfreelist,head);
            zfree(o);
        } else {//删除过期健
            int j, k, freed = 0;

            for (j = 0; j < server.dbnum; j++) {
                int minttl = -1;
                robj *minkey = NULL;
                struct dictEntry *de;

                if (dictSize(server.db[j].expires)) {
                    freed = 1;
                    /* From a sample of three keys drop the one nearest to
                     * the natural expire */
                    for (k = 0; k < 3; k++) {
                        time_t t;

                        de = dictGetRandomKey(server.db[j].expires);
                        t = (time_t) dictGetEntryVal(de);
                        if (minttl == -1 || t < minttl) {
                            minkey = dictGetEntryKey(de);
                            minttl = t;
                        }
                    }
                    deleteKey(server.db+j,minkey);
                }
            }
            if (!freed) return; /* nothing to free... */
        }
    }
}

/* ================================= Debugging ============================== */
//debug
static void debugCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"segfault")) {//使程序崩溃
        *((char*)-1) = 'x';
    } else if (!strcasecmp(c->argv[1]->ptr,"object") && c->argc == 3) {
        dictEntry *de = dictFind(c->db->dict,c->argv[2]);
        robj *key, *val;

        if (!de) {
            addReply(c,shared.nokeyerr);
            return;
        }
        key = dictGetEntryKey(de);
        val = dictGetEntryVal(de);
        addReplySds(c,sdscatprintf(sdsempty(),
            "+Key at:%p refcount:%d, value at:%p refcount:%d\r\n",
                key, key->refcount, val, val->refcount));
    } else {
        addReplySds(c,sdsnew(
            "-ERR Syntax error, try DEBUG [SEGFAULT|OBJECT <key>]\r\n"));
    }
}

#ifdef HAVE_BACKTRACE
static struct redisFunctionSym symsTable[] = {
{"freeStringObject", (unsigned long)freeStringObject},
{"freeListObject", (unsigned long)freeListObject},
{"freeSetObject", (unsigned long)freeSetObject},
{"decrRefCount", (unsigned long)decrRefCount},
{"createObject", (unsigned long)createObject},
{"freeClient", (unsigned long)freeClient},
{"rdbLoad", (unsigned long)rdbLoad},
{"addReply", (unsigned long)addReply},
{"addReplySds", (unsigned long)addReplySds},
{"incrRefCount", (unsigned long)incrRefCount},
{"rdbSaveBackground", (unsigned long)rdbSaveBackground},
{"createStringObject", (unsigned long)createStringObject},
{"replicationFeedSlaves", (unsigned long)replicationFeedSlaves},
{"syncWithMaster", (unsigned long)syncWithMaster},
{"tryObjectSharing", (unsigned long)tryObjectSharing},
{"removeExpire", (unsigned long)removeExpire},
{"expireIfNeeded", (unsigned long)expireIfNeeded},
{"deleteIfVolatile", (unsigned long)deleteIfVolatile},
{"deleteKey", (unsigned long)deleteKey},
{"getExpire", (unsigned long)getExpire},
{"setExpire", (unsigned long)setExpire},
{"updateSlavesWaitingBgsave", (unsigned long)updateSlavesWaitingBgsave},
{"freeMemoryIfNeeded", (unsigned long)freeMemoryIfNeeded},
{"authCommand", (unsigned long)authCommand},
{"pingCommand", (unsigned long)pingCommand},
{"echoCommand", (unsigned long)echoCommand},
{"setCommand", (unsigned long)setCommand},
{"setnxCommand", (unsigned long)setnxCommand},
{"getCommand", (unsigned long)getCommand},
{"delCommand", (unsigned long)delCommand},
{"existsCommand", (unsigned long)existsCommand},
{"incrCommand", (unsigned long)incrCommand},
{"decrCommand", (unsigned long)decrCommand},
{"incrbyCommand", (unsigned long)incrbyCommand},
{"decrbyCommand", (unsigned long)decrbyCommand},
{"selectCommand", (unsigned long)selectCommand},
{"randomkeyCommand", (unsigned long)randomkeyCommand},
{"keysCommand", (unsigned long)keysCommand},
{"dbsizeCommand", (unsigned long)dbsizeCommand},
{"lastsaveCommand", (unsigned long)lastsaveCommand},
{"saveCommand", (unsigned long)saveCommand},
{"bgsaveCommand", (unsigned long)bgsaveCommand},
{"shutdownCommand", (unsigned long)shutdownCommand},
{"moveCommand", (unsigned long)moveCommand},
{"renameCommand", (unsigned long)renameCommand},
{"renamenxCommand", (unsigned long)renamenxCommand},
{"lpushCommand", (unsigned long)lpushCommand},
{"rpushCommand", (unsigned long)rpushCommand},
{"lpopCommand", (unsigned long)lpopCommand},
{"rpopCommand", (unsigned long)rpopCommand},
{"llenCommand", (unsigned long)llenCommand},
{"lindexCommand", (unsigned long)lindexCommand},
{"lrangeCommand", (unsigned long)lrangeCommand},
{"ltrimCommand", (unsigned long)ltrimCommand},
{"typeCommand", (unsigned long)typeCommand},
{"lsetCommand", (unsigned long)lsetCommand},
{"saddCommand", (unsigned long)saddCommand},
{"sremCommand", (unsigned long)sremCommand},
{"smoveCommand", (unsigned long)smoveCommand},
{"sismemberCommand", (unsigned long)sismemberCommand},
{"scardCommand", (unsigned long)scardCommand},
{"spopCommand", (unsigned long)spopCommand},
{"sinterCommand", (unsigned long)sinterCommand},
{"sinterstoreCommand", (unsigned long)sinterstoreCommand},
{"sunionCommand", (unsigned long)sunionCommand},
{"sunionstoreCommand", (unsigned long)sunionstoreCommand},
{"sdiffCommand", (unsigned long)sdiffCommand},
{"sdiffstoreCommand", (unsigned long)sdiffstoreCommand},
{"syncCommand", (unsigned long)syncCommand},
{"flushdbCommand", (unsigned long)flushdbCommand},
{"flushallCommand", (unsigned long)flushallCommand},
{"sortCommand", (unsigned long)sortCommand},
{"lremCommand", (unsigned long)lremCommand},
{"infoCommand", (unsigned long)infoCommand},
{"mgetCommand", (unsigned long)mgetCommand},
{"monitorCommand", (unsigned long)monitorCommand},
{"expireCommand", (unsigned long)expireCommand},
{"getSetCommand", (unsigned long)getSetCommand},
{"ttlCommand", (unsigned long)ttlCommand},
{"slaveofCommand", (unsigned long)slaveofCommand},
{"debugCommand", (unsigned long)debugCommand},
{"processCommand", (unsigned long)processCommand},
{"setupSigSegvAction", (unsigned long)setupSigSegvAction},
{"readQueryFromClient", (unsigned long)readQueryFromClient},
{"rdbRemoveTempFile", (unsigned long)rdbRemoveTempFile},
{NULL,0}
};
//此函数尝试将指针转换为函数名。
/* This function try to convert a pointer into a function name. It's used in
 * oreder to provide a backtrace under segmentation fault that's able to
 * display functions declared as static (otherwise the backtrace is useless). */
static char *findFuncName(void *pointer, unsigned long *offset){
    int i, ret = -1;
    unsigned long off, minoff = 0;

    /* Try to match against the Symbol with the smallest offset */
    for (i=0; symsTable[i].pointer; i++) {
        unsigned long lp = (unsigned long) pointer;

        if (lp != (unsigned long)-1 && lp >= symsTable[i].pointer) {
            off=lp-symsTable[i].pointer;
            if (ret < 0 || off < minoff) {
                minoff=off;
                ret=i;
            }
        }
    }
    if (ret == -1) return NULL;
    *offset = minoff;
    return symsTable[ret].name;
}

static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
    return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
    return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
    return (void*) uc->uc_mcontext->__ss.__eip;
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  #ifdef _STRUCT_X86_THREAD_STATE64
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__i386__) || defined(__X86_64__) /* Linux x86 */
    return (void*) uc->uc_mcontext.gregs[REG_EIP];
#elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}
//它是一个信号处理函数，用于处理 Redis 服务器中发生的段错误信号。
//该函数通过捕获信号信息、记录服务器的当前状态、生成并打印堆栈跟踪信息，以及最后优雅地退出程序来工作
static void segvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    unsigned long offset=0;
    time_t uptime = time(NULL)-server.stat_starttime;
    ucontext_t *uc = (ucontext_t*) secret;
    REDIS_NOTUSED(info);

    redisLog(REDIS_WARNING,
        "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
    redisLog(REDIS_WARNING, "%s", sdscatprintf(sdsempty(),
        "redis_version:%s; "
        "uptime_in_seconds:%d; "
        "connected_clients:%d; "
        "connected_slaves:%d; "
        "used_memory:%zu; "
        "changes_since_last_save:%lld; "
        "bgsave_in_progress:%d; "
        "last_save_time:%d; "
        "total_connections_received:%lld; "
        "total_commands_processed:%lld; "
        "role:%s;"
        ,REDIS_VERSION,
        uptime,
        listLength(server.clients)-listLength(server.slaves),
        listLength(server.slaves),
        server.usedmemory,
        server.dirty,
        server.bgsaveinprogress,
        server.lastsave,
        server.stat_numconnections,
        server.stat_numcommands,
        server.masterhost == NULL ? "master" : "slave"
    ));

    trace_size = backtrace(trace, 100);
    /* overwrite sigaction with caller's address */
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);

    for (i=1; i<trace_size; ++i) {
        char *fn = findFuncName(trace[i], &offset), *p;

        p = strchr(messages[i],'+');
        if (!fn || (p && ((unsigned long)strtol(p+1,NULL,10)) < offset)) {
            redisLog(REDIS_WARNING,"%s", messages[i]);
        } else {
            redisLog(REDIS_WARNING,"%d redis-server %p %s + %d", i, trace[i], fn, (unsigned int)offset);
        }
    }
    free(messages);
    exit(0);
}
//为几个关键的信号（段错误、总线错误、浮点异常和非法指令）设置信号处理函数。
static void setupSigSegvAction(void) {
    struct sigaction act;

    sigemptyset (&act.sa_mask);
    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = segvHandler;
    sigaction (SIGSEGV, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    sigaction (SIGFPE, &act, NULL);
    sigaction (SIGILL, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    return;
}
#else /* HAVE_BACKTRACE */
static void setupSigSegvAction(void) {
}
#endif /* HAVE_BACKTRACE */

/* =================================== Main! ================================ */

#ifdef __linux__
//这个函数 linuxOvercommitMemoryValue 的目的是读取 Linux 系统中的 /proc/sys/vm/overcommit_memory 文件的内容，该文件包含了内存超额提交（overcommit）策略的值。
//超额提交是指系统允许分配的内存总量超过物理内存和交换空间（swap space）的总和。这个函数将读取到的值转换为整数并返回。
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxOvercommitMemoryWarning(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        redisLog(REDIS_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low condition memory. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
}
#endif /* __linux__ */

static void daemonize(void) {
    int fd;
    FILE *fp;

    if (fork() != 0) exit(0); /* parent exits */
    //调用setsid()函数创建一个新的会话，并成为该会话的会话首进程（session leader）。这会导致进程脱离控制终端，并成为一个新的进程组的组长。
    //这是守护进程化的一个重要步骤，因为它确保了进程不会接收到任何来自终端的信号。
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        //使用dup2()函数将标准输入（STDIN_FILENO）、标准输出（STDOUT_FILENO）和标准错误（STDERR_FILENO）
        //都重定向到/dev/null。这样，守护进程就不会产生任何输出到终端或日志文件
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    /* Try to write the pid file */
    fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",getpid());
        fclose(fp);
    }
}

int main(int argc, char **argv) {
    initServerConfig();//初始化config
    if (argc == 2) {
        ResetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
    } else {
        redisLog(REDIS_WARNING,"Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    initServer();
    if (server.daemonize) daemonize();
    redisLog(REDIS_NOTICE,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    if (rdbLoad(server.dbfilename) == REDIS_OK)
        redisLog(REDIS_NOTICE,"DB loaded from disk");
    if (aeCreateFileEvent(server.el, server.fd, AE_READABLE,
        acceptHandler, NULL) == AE_ERR) oom("creating file event");
    redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}
