zmalloc.h zmalloc.c编写了一些内存分配函数，主要是对molloc 和realloc的封装

sds.h和sds.c定义了一个新的字符串类型，兼容部分string.h函数，是对char *类型的升级，新结构体可以保存char*长度和可用内存
sds.c中定义了一系列sds有关的函数，例如生成一个销毁一个sds，格式化增长，分割sds等操作
