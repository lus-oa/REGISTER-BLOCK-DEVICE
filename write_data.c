#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main()
{
    // 打开设备文件
    int fd = open("/dev/blockdev", O_WRONLY);
    if (fd == -1)
    {
        perror("Failed to open /dev/blockdev");
        return 1;
    }

    // 写入数据
    const int buffer_size = 1024 * 1024; // 1MB 缓冲区大小
    char *buffer = (char *)malloc(buffer_size);
    if (buffer == NULL)
    {
        perror("Failed to allocate buffer");
        close(fd);
        return 1;
    }

    // 填充缓冲区，模拟写入的数据
    for (int i = 0; i < buffer_size; ++i)
    {
        buffer[i] = (char)(i % 256);
    }

    // 写入数据到设备
    int total_bytes_written = 0;
    while (total_bytes_written < 1024 * 1024)
    { // 写入 1MB 数据
        int bytes_written = write(fd, buffer, buffer_size);
        if (bytes_written == -1)
        {
            perror("Write error");
            break;
        }
        total_bytes_written += bytes_written;
    }

    printf("Total bytes written: %d\n", total_bytes_written);

    // 关闭设备文件和释放缓冲区
    close(fd);
    free(buffer);

    return 0;
}
