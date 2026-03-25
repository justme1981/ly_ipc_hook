#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>

// 模拟原函数指针
typedef int (*orig_func_t)(void *stream);

orig_func_t orig_func = NULL;

// 简单 dump 文件
int fd = -1;

int IMP_Encoder_GetStream(void *stream) {
    if (!orig_func) {
        orig_func = (orig_func_t)dlsym(RTLD_NEXT, "IMP_Encoder_GetStream");
    }

    int ret = orig_func(stream);

    // 👉 这里假设 stream 前面就是 H264（实际后面可优化）
    if (fd < 0) {
        fd = open("/tmp/dump.h264", O_CREAT | O_WRONLY | O_APPEND, 0777);
    }

    if (fd >= 0) {
        write(fd, stream, 4096);  // 简单写（后续可改成真实 size）
    }

    return ret;
}
