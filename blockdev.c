#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#define FILE_PATH "/bio.txt"
static int dev_major = 0;

/* Just internal representation of the our block device
 * can hold any useful data */
struct block_dev
{
    sector_t capacity;
    u8 *data; /* Data buffer to emulate real storage device */
    struct blk_mq_tag_set tag_set;
    struct request_queue *queue;
    struct gendisk *gdisk;
};

/* Device instance */
static struct block_dev *block_device = NULL;

struct rdmareq
{
    // rw_flag读写标志。0：读；1：写；-1：其他操作
    int rw_flag;
    sector_t sector;
    unsigned int totaldata_len;
    void *virtaddr;
    // phys_addr_t physaddr;
    unsigned int partlen;
};

static int blockdev_open(struct block_device *dev, fmode_t mode)
{
    printk(">>> blockdev_open\n");

    return 0;
}

static void blockdev_release(struct gendisk *gdisk, fmode_t mode)
{
    printk(">>> blockdev_release\n");
}

int blockdev_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg)
{
    printk("ioctl cmd 0x%08x\n", cmd);

    return -ENOTTY;
}

/* Set block device file I/O */
static struct block_device_operations blockdev_ops = {
    .owner = THIS_MODULE,
    .open = blockdev_open,
    .release = blockdev_release,
    .ioctl = blockdev_ioctl};

/* Serve requests */
static int do_request(struct request *rq, unsigned int *nr_bytes)
{
    int ret = 0;
    struct bio_vec bvec;
    struct req_iterator iter;
    struct block_dev *dev = rq->q->queuedata;
    loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
    loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT);

    // printk(KERN_WARNING "sblkdev: request start from sector %lld  pos = %lld  dev_size = %lld\n", blk_rq_pos(rq), pos, dev_size);

    /* Iterate over all requests segments */
    rq_for_each_segment(bvec, rq, iter)
    {
        unsigned long b_len = bvec.bv_len;

        /* Get pointer to the data */
        void *b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

        /* Simple check that we are not out of the memory bounds */
        if ((pos + b_len) > dev_size)
        {
            b_len = (unsigned long)(dev_size - pos);
        }

        if (rq_data_dir(rq) == WRITE)
        {
            /* Copy data to the buffer in to required position */
            memcpy(dev->data + pos, b_buf, b_len);
        }
        else
        {
            /* Read data from the buffer's position */
            memcpy(b_buf, dev->data + pos, b_len);
        }

        /* Increment counters */
        pos += b_len;
        *nr_bytes += b_len;
    }

    return ret;
}

void write_bio(const struct rdmareq *req)
{
    struct file *file;
    loff_t pos = 0;
    char hex_string[256];

    file = filp_open(FILE_PATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (IS_ERR(file))
    {
        printk(KERN_ERR "Failed to open file\n");
        return;
    }

    // 将结构体字段以16进制形式写入字符串，没有字段名和分隔符
    snprintf(hex_string, sizeof(hex_string), "%x%llx%x%p%x\n",
             req->rw_flag, (unsigned long long)req->sector, req->totaldata_len,
             req->virtaddr, req->partlen);

    // 使用 kernel_write 写入数据
    kernel_write(file, hex_string, strlen(hex_string), &pos);
    filp_close(file, NULL);
}

void print_request(struct request *rq)
{
    struct bio_vec bvec;
    struct req_iterator iter;
    int rw;

    /*
       REQ_WRITE 标志位用于表示写操作
       REQ_READ 标志位用于表示读操作
    */
    if (rq_data_dir(rq) == READ)
    {
        // 读取操作
        rw = 0;
    }
    else if (rq_data_dir(rq) == WRITE)
    {
        // 写入操作
        rw = 1;
    }
    else
    {
        // 其他操作
        rw = -1;
    }

    rq_for_each_bvec(bvec, rq, iter)
    {
        struct rdmareq rrq;
        struct page *page = bvec.bv_page;
        static char *kernel_buffer = NULL;
        unsigned int offset = bvec.bv_offset;
        unsigned int len = bvec.bv_len;
        int buffer_size = 4096; // 根据需要设置缓冲区大小
        kernel_buffer = kmalloc(buffer_size, GFP_KERNEL);
        if (!kernel_buffer)
        {
            printk(KERN_ERR "Failed to allocate kernel buffer.\n");
            return;
        }

        // 初始化内核缓冲区
        memset(kernel_buffer, 0, buffer_size);
        rrq.rw_flag = rw;
        rrq.sector = blk_rq_pos(rq);
        rrq.totaldata_len = blk_rq_bytes(rq);
        // 地址
        rrq.virtaddr = page_address(page) + offset;
        rrq.partlen = len;
        memcpy(kernel_buffer, rrq.virtaddr, len);

        // 分配用户态缓冲区，假设缓冲区大小为 len
        char *user_buffer = (char *)kmalloc(strlen(kernel_buffer) + 1, GFP_KERNEL);
        printk("Allocate user buffer successfully.\n");

        if (user_buffer == NULL)
        {
            // 内存分配失败，处理错误
            printk(KERN_ERR "Failed to allocate user buffer.\n");
            return;
        }

        // 使用 copy_to_user 将数据从内核虚拟地址复制到用户态缓冲区
        if (copy_to_user(user_buffer, kernel_buffer, strlen(kernel_buffer) + 1))
        {
            // 复制失败，处理错误
            printk(KERN_ERR "Failed to copy data to user space.\n");
            kfree(user_buffer); // 释放分配的内存
            kfree(kernel_buffer);
            return;
        }

        // 在这里处理当前 bvec 的物理地址
        // 例如，打印物理地址或执行其他操作
        printk(KERN_INFO "Physical Address: %p, Partlen: %u\n",
               rrq.virtaddr, len);

        // 使用完用户态缓冲区后，记得释放分配的内存
        kfree(user_buffer);
        kfree(kernel_buffer);
    }
}

static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;

    if (strcmp(current->comm, "write_data") == 0)
    {
        print_request(rq);
    }

    /* Start request serving procedure */
    blk_mq_start_request(rq);

    if (do_request(rq, &nr_bytes) != 0)
    {
        status = BLK_STS_IOERR;
    }

    /* Notify kernel about processed nr_bytes */
    if (blk_update_request(rq, status, nr_bytes))
    {
        /* Shouldn't fail */
        BUG();
    }

    /* Stop request serving procedure */
    __blk_mq_end_request(rq, status);

    return status;
}

static struct blk_mq_ops mq_ops = {
    .queue_rq = queue_rq,
};

static int __init myblock_driver_init(void)
{
    /* Register new block device and get device major number */
    dev_major = register_blkdev(dev_major, "testblk");

    block_device = kmalloc(sizeof(struct block_dev), GFP_KERNEL);

    if (block_device == NULL)
    {
        printk("Failed to allocate struct block_dev\n");
        unregister_blkdev(dev_major, "testblk");

        return -ENOMEM;
    }

    /* Set some random capacity of the device */
    block_device->capacity = (11200 * PAGE_SIZE) >> 9; /* nsectors * SECTOR_SIZE; */
    /* Allocate corresponding data buffer */
    block_device->data = kmalloc(112 * PAGE_SIZE, GFP_KERNEL);

    if (block_device->data == NULL)
    {
        printk("Failed to allocate device IO buffer\n");
        unregister_blkdev(dev_major, "testblk");
        kfree(block_device);

        return -ENOMEM;
    }

    printk("Initializing queue\n");

    block_device->queue = blk_mq_init_sq_queue(&block_device->tag_set, &mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);

    if (block_device->queue == NULL)
    {
        printk("Failed to allocate device queue\n");
        kfree(block_device->data);

        unregister_blkdev(dev_major, "testblk");
        kfree(block_device);

        return -ENOMEM;
    }

    /* Set driver's structure as user data of the queue */
    block_device->queue->queuedata = block_device;

    /* Allocate new disk */
    block_device->gdisk = alloc_disk(1);

    /* Set all required flags and data */
    block_device->gdisk->flags = GENHD_FL_NO_PART_SCAN;
    block_device->gdisk->major = dev_major;
    block_device->gdisk->first_minor = 0;

    block_device->gdisk->fops = &blockdev_ops;
    block_device->gdisk->queue = block_device->queue;
    block_device->gdisk->private_data = block_device;

    /* Set device name as it will be represented in /dev */
    strncpy(block_device->gdisk->disk_name, "blockdev\0", 9);

    printk("Adding disk %s\n", block_device->gdisk->disk_name);

    /* Set device capacity */
    set_capacity(block_device->gdisk, block_device->capacity);

    /* Notify kernel about new disk device */
    add_disk(block_device->gdisk);

    return 0;
}

static void __exit myblock_driver_exit(void)
{
    /* Don't forget to cleanup everything */
    if (block_device->gdisk)
    {
        del_gendisk(block_device->gdisk);
        put_disk(block_device->gdisk);
    }

    if (block_device->queue)
    {
        blk_cleanup_queue(block_device->queue);
    }

    kfree(block_device->data);

    unregister_blkdev(dev_major, "testblk");
    kfree(block_device);
}

module_init(myblock_driver_init);
module_exit(myblock_driver_exit);
MODULE_LICENSE("GPL");
