//掌握：①解串器工作的大致流程
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <media/camera_common.h>
#include <linux/module.h>
#include <linux/kthread.h>   // kthread APIs
#include <linux/delay.h>     // msleep
#include <linux/sched.h>     // TASK_INTERRUPTIBLE
#include "r_max96712.h"

#define MAX96712_TX11_PIPE_X_EN_ADDR 0x90B//管道使能
#define MAX96712_TX45_PIPE_X_DST_CTRL_ADDR 0x92D//目标端口映射
#define MAX96712_PIPE_X_SRC_0_MAP_ADDR 0x90D//数据源映射


#define MAX96712_PHY_CLK (0x20|25)

//在入口函数之前，所有的函数定义都只是框架模型

///②定义一个解串器结构体，里面定义的iic和寄存器以及互斥锁结构体，就像是一个通行证，通过它获取
//一个身份，能够得到对应的资源，因而具备身份所带来的特征
struct max96712 {
	struct i2c_client *i2c_client;
	struct regmap *regmap;
	struct mutex lock;
	u8 link_lock_status;
	//这个字段是 max96712 芯片的链路锁定状态寄存器 / 状态标记
	u8 ismaster;
	struct task_struct *anomaly_thread;
	//异常监测线程
	bool des_link_start_stream[4];
	//4 路链路是否已经开启码流
};

//检查空指针
#define CHECKNULL(x) if(x==NULL){ \
		dev_err(NULL, "zuojisi %s: ##x## NULL ptr\n",__func__); \
		return -EINVAL; \
		}

static int vercheck=0;
static int check_version(struct i2c_client *client);
static int check_device_id_rev(struct i2c_client *client);
// static int check_link(struct i2c_client *client);

static int max96712_write_reg(struct device *dev,
	u16 addr, u8 val)
{
	struct max96712 *priv;
	int err;

	priv = dev_get_drvdata(dev);
	//从 dev 中取出你定义的私有数据：struct max96712

	err = regmap_write(priv->regmap, addr, val);
	//成功返回0，失败返回负数
	if (err)
		dev_err(dev,
		"zuojisi %s:i2c write failed, 0x%x = %x\n",
		__func__, addr, val);

	/* delay before next i2c command as required for SERDES link */
	usleep_range(100, 110);//须延时 100us
	if(!err)
	//addr 是 u16 类型 → 16 位整数；val 是 u8 类型 → 8 位整数
	//1 位十六进制数 = 4 位二进制数（2⁴=16）；
	//u8 是 8 位二进制 → 8/4 = 2 位十六进制（范围：0x00 ~ 0xFF）；
	//u16 是 16 位二进制 → 16/4 = 4 位十六进制（范围：0x0000 ~ 0xFFFF）
		dev_info(dev,"zuojisi:96712:w addr(0x%04x)->0x%02x\n",addr,val);

	return err;
}

static int max96712_read_reg(struct device *dev,
	u16 addr, u32* val)
{
	struct max96712 *priv;
	int err;

	priv = dev_get_drvdata(dev);

	err = regmap_read(priv->regmap, addr, val);
	if (err)
		dev_err(dev,
		"zuojisi %s:i2c read failed, 0x%x\n",
		__func__, addr);

	/* delay before next i2c command as required for SERDES link */
	usleep_range(100, 110);
	if(!err)
		dev_info(dev,"zuojisi:96712:r addr(0x%04x)<-0x%02x\n",addr,(u8)(*val));
	return err;
}

int z_max96712_lock_link(struct device *dev)
{
	struct max96712 *priv = NULL;

	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	mutex_lock(&priv->lock);//加锁
	dev_info(dev,"zuojisi: mutex lock");
	return 0;
}
EXPORT_SYMBOL(z_max96712_lock_link);

int z_max96712_unlock_link(struct device *dev)
{
	struct max96712 *priv = NULL;

	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev,"zuojisi: mutex unlock");
	mutex_unlock(&priv->lock);//解锁
	return 0;
}
EXPORT_SYMBOL(z_max96712_unlock_link);

int z_max96712_monopolize_link(struct device *dev, int link)//独占链路，只监听一条链路
{
	struct max96712 *priv = NULL;
	u32 value;

	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev, "zuojisi: %s: monopolize link_%c\n", __func__, 'A'+link);
	//是一个ASCII 字符计算，用来把数字链路号转换成对应的字母链路名，方便日志打印。
	max96712_read_reg(dev,0x0006,&value);
	//0x06是一个8位寄存器，高四位代表的gmsl2模式下的四条链路，低四位则对应了每个链路的使能
	value = (value & 0xF0) | (1<<link);
	//高四位保留，低四位清零，因为link<=4,所以实际效果只影响低四位，或操作将其拼接
	//效果：传递的参数即为单独使能的链路
	max96712_write_reg(dev,0x0006,value);
	mdelay(150);
	return 0;
}
EXPORT_SYMBOL(z_max96712_monopolize_link);

int z_max96712_enable_link(struct device *dev, int link)
//参数link：0、1、2、3
{
    struct max96712 *priv = NULL;
 
    CHECKNULL(dev);
    priv = dev_get_drvdata(dev);
    CHECKNULL(priv);
 
    dev_info(dev, "zuojisi: %s: enable link_%c\n", __func__, 'A'+link);
    priv->link_lock_status |= (1<<link);
	//把状态表示的对应位置置1
	//使能链路，不用改变寄存器的值吗？直接把标记状态置1就完成了？
	//后续会有代码启动映射条目和管道，这只是更改标记
    return 0;
}
EXPORT_SYMBOL(z_max96712_enable_link);

int z_max96712_restore_link(struct device *dev)//让芯片恢复使用所有已经连接好的摄像头链路
{
	struct max96712 *priv = NULL;
	u32 value;

	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev, "zuojisi: restore links\n");
	max96712_read_reg(dev,0x0006,&value);
	max96712_write_reg(dev,0x0006,(value&0xF0)|(priv->link_lock_status&0x0F));
	//保留高4位，拼接（保留低四位），取消独占链路，
	//关键在于link_lock_statue保留了原本的链路状态
	mdelay(150);
	return 0;
}
EXPORT_SYMBOL(z_max96712_restore_link);

int z_max96712_check_link_status(struct device *dev, int link)//查询指定链路状态
{
	struct max96712 *priv = NULL;
	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);
	if(priv->link_lock_status & (0x1<<link))
	//右边：(1 << link)（掩码，只有 1 位是 1）
	{
		return 1;
	}else{
		return 0;
	}
}
EXPORT_SYMBOL(z_max96712_check_link_status);

int z_max96712_set_link_bandwidth(struct device *dev, int link, int gbps)//设置 GMSL 摄像头链路的传输速度
//gbps：速度（3 或 6）
//在速率控制的时候，能看出A和B是绑定的
{
	struct max96712 *priv = NULL;
	u32 value;
	//根据下面的偏移量操作，应该是每四位代表一条链路
	u16 reg = 0x0010;
	//该寄存器表示链路A\B的发送和接收速率
	u8 regval = 0x01;//数据手册里0x01表示3Gbps
	if(gbps!=3){ //
		regval = 0x02;//2'b0x10
	}

	CHECKNULL(dev);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev, "zuojisi: set link_%c to %dGbps\n",link+'A',gbps);
	if(link>=2)
	//假设是3后，reg变成0x0011,表示链路C\D的发送和接收速率；link变成1/link变成2（4）
	{
		reg++, link-=2;
	}
	max96712_read_reg(dev,reg,&value);
	//读取寄存器的值,value是32位，
	value = (value & (~(0x3<<(link*4)))) | (regval<<(link*4));  // 将对应链路的四位，清零，并改成设置的速率，偏移量是4，因为两个速率寄存器是八位的
	//分别控制A/B/C/D,四位负责一位
	//0x3表示什么0011将设置速率的两位置1，取反后清0，然后或拼接成设置的置，即改为对应的速率
	max96712_write_reg(dev,reg,value & 0xFF);//value & 0xFF 的作用：只保留低 8 位，丢弃高位,value和reg的数字范围不同（32和16）
	//reg 是寄存器地址（16 位），但寄存器本身是 8 位 的
	max96712_write_reg(dev,0x0018,(1<<link));//低 4 位：RESET_ONESHOT_x（bit3~0）
	//工作方式：写 1 触发复位，硬件会自动清零该位，自动释放复位。
	//前面更改寄存器的值只是软设置，只有进行复位后才会生效？

	mdelay(300);
	return 0;
}
EXPORT_SYMBOL(z_max96712_set_link_bandwidth);

static int max96712_setup_pipeline(struct device *dev,struct gmsl_link_ctx *g_ctx)//重点
//1 条解串器链路（Link） = 占用 1 个管道（Pipe）对应 4个 VC（虚拟通道），对应8条映射条目（可以为不同 VC 配置不同的映射条目）
//构建管道：启用映射条目，控制器，虚拟通道和数据帧格式
//MIPI CSI-2 控制器，也就是芯片里专门用来接收 MIPI CSI-2 格式视频数据的硬件模块，目的地
{
	struct max96712 *priv = NULL;
	int pipe_id = 0;
	u32 i = 0;
	//i为什么是32？4×8=32？是对应管道还是对应映射条目
	u8 dst_vc = 0,src_vc = 0;


	CHECKNULL(dev);
	CHECKNULL(g_ctx);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev,"zuojisi:96712: num_streams=%d\n",g_ctx->num_streams);
	//dev,设备指针

	for (i = 0; i < g_ctx->num_streams; i++)
	//streams对应的是数据流，那应该最大为7
	{
		pipe_id = g_ctx->des_link;
		//所有的寄存器操作，都是用同一个 pipe_id 来算地址的：
		//管道 ID = 解串器链路号，pipe_id 就是当前配置第几路管道
		dev_info(dev, "zuojisi: pipe_%d receive stream_id=%d, datatype=0x%x\n", pipe_id,i,g_ctx->streams[i]);

		//dst_vc = (g_ctx->des_link==0 || g_ctx->des_link==2) ? 0:1; //for link B and linkD set vc=2
		dst_vc = g_ctx->des_link;
		//设置 VC（虚拟通道）

		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_TX11_PIPE_X_EN_ADDR+0, 0x1f);
		//驱动里习惯把这个寄存器叫做 “管道使能”，本质上是通过启用映射条目，来让管道真正开始工作
		//每个管道的寄存器组，都占用 0x40（64 字节）的地址空间，并且是连续排布的。
		//设备是怎么排布的每个都是基地址+寄存器地址+偏移量地址？每个寄存器上有4个寄存器组？
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_TX45_PIPE_X_DST_CTRL_ADDR+0, 0x55);
		//结果是所有的映射条目全部输出到CSI controller 1
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_TX45_PIPE_X_DST_CTRL_ADDR+1, 0x01); 
		//ADDR +0 → MIPI_TX45 → 控制 SRC0 ~ SRC3
		//ADDR+1→ MIPI_TX46 → 控制 SRC4 ~ SRC7
		//结果是只让 SRC4 走 CSI1 输出，其他 SRC5~7 都走 CSI0（默认）
		//数据流映射表（10 个连续寄存器）
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+0, g_ctx->streams[i] | (src_vc << 6));
		//src_vc << 6：占据 bit7~bit6，设置虚拟通道。
        //g_ctx->streams[i]：占据 bit5~bit0，设置数据流 ID（Data Type）
		//根据寄存器的规定，猜测g_ctx->streams[i]应该是6位
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+1, g_ctx->streams[i] | (dst_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+2, 0x00 | (src_vc << 6));
		//0x00 是 MIPI CSI-2 协议里的「帧同步码 FS (Frame Sync)」数据流类型
		//当前摄像头真正发送的【图像数据类型】（RAW8/RAW10/RAW12 等）
		//是图像数据类型，只存在于数据负载包，所以只有前两句用
		//后面的 FS/FE/LS/LE 是控制包，类型是协议固定的，直接写死 0x00~0x03 即可
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+3, 0x00 | (dst_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+4, 0x01 | (src_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+5, 0x01 | (dst_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+6, 0x02 | (src_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+7, 0x02 | (dst_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+8, 0x03 | (src_vc << 6));
		max96712_write_reg(dev, (0x40*pipe_id)+MAX96712_PIPE_X_SRC_0_MAP_ADDR+9, 0x03 | (dst_vc << 6));	
		//每个映射条目占两个寄存器（源 + 目标）
		//0x01	FE (Frame End)	帧结束包	一帧图像结束的标记
		//0x02	LS (Line Sync)	行同步包	一行图像数据开始的标记
		//0x03	LE (Line End)	行结束包	一行图像数据结束的标记
		//每个数据的源都是0，dst_vc则在一次循环中固定等于链路号
	}
	//是不是由串行数据重新变为并行输出了
	//写的时候寄存器是挨着的，感觉像串行
	//一帧图像 = FS + [ （LS + 一行像素数据 + LE） × 图像行数 ] + FE
	//图像数据长包：只负责传像素内容，不包含同步信息。
	//FS/FE/LS/LE 短包：只负责同步控制，不携带像素数据。
	return 0;
}

int z_max96712_start_streaming(struct device *dev, struct gmsl_link_ctx *g_ctx)
//加锁、启动管道、复位、校准、解锁
{
	struct max96712 *priv = NULL;
	int err;
	u32 val;
	
	CHECKNULL(dev);
	CHECKNULL(g_ctx);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev,"zuojisi: 96712 start streaming, link=%c - enter\n",'A'+g_ctx->des_link);

	mutex_lock(&priv->lock);
	err = max96712_setup_pipeline(dev, g_ctx);
	if (err)
		return err;

	max96712_read_reg(dev,0xf4,&val);
	//启动管道，与之前的启动映射条目不是同一级，
	max96712_write_reg(dev,0xf4, (val | (1<<g_ctx->des_link))&0xff );

	// //reset mipi phy
	// if(s_priv->des_link==2 || s_priv->des_link==3){
	// 	max96712_write_reg(dev,0x8a2, 0x30);
	// }else{
	// 	max96712_write_reg(dev,0x8a2, 0xc0);
	// }
	// max96712_write_reg(dev,0x8a2, 0xf0);

	// reset link
	max96712_write_reg(dev,0x018, (1<<g_ctx->des_link));
	//写入一次性复位
	usleep_range(50*1000, 50*1000+1000);
	//休眠，单位是微秒，50毫秒到51毫秒
	mdelay(100);
	
	//skew
	if(g_ctx->des_link==0 || g_ctx->des_link==1
		//MIPI D-PHY 去斜（Deskew）初始校准控制，也就是调整 MIPI 数据 / 时钟线的相位，解决花屏、失步问题。
		{
		max96712_write_reg(dev,0x943,0x10);
		//0x10 = 进入手动校准模式，准备开始校准
		max96712_write_reg(dev,0x943,0x33);
		//正式触发校准，并设置最佳校准宽度
	}else{
		//C/D 的
		max96712_write_reg(dev,0x983,0x10);
		max96712_write_reg(dev,0x983,0x33);
	}

	mutex_unlock(&priv->lock);
	dev_info(dev,"zuojisi: 96712 start streaming - exit\n");
	priv->des_link_start_stream[g_ctx->des_link] = true;
	//在驱动内存里，记一笔账：
	//“第 X 条链路已经成功启动流了”
	return 0;
}
EXPORT_SYMBOL(z_max96712_start_streaming);

int z_max96712_stop_streaming(struct device *dev, struct gmsl_link_ctx *g_ctx)
{
	struct max96712 *priv = NULL;
	u32 val;
	u8 tmp;

	CHECKNULL(dev);
	CHECKNULL(g_ctx);
	priv = dev_get_drvdata(dev);
	CHECKNULL(priv);

	dev_info(dev,"zuojisi: 96712 stop streaming, link_%c - enter\n",'A'+g_ctx->des_link);
	mutex_lock(&priv->lock);

	max96712_read_reg(dev,0xf4,&val);
	//读取启动的管道
	tmp = (val & (~(1<<g_ctx->des_link))) & 0xff;
	//更改数值，把对应链路位置的值置0，停止管道，保留低八位
	max96712_write_reg(dev,0xf4, tmp );
	if(tmp==0)
	//所有管道全部停止
	{
		max96712_write_reg(dev,0x018, 0x0f);
		//全部一次性复位
		dev_info(dev,"zuojisi: all stopped, reset links\n");
		mdelay(30);
	}

	mutex_unlock(&priv->lock);
	dev_info(dev,"zuojisi: 96712 stop streaming - exit\n");
	priv->des_link_start_stream[g_ctx->des_link] = false;
	return 0;
}
EXPORT_SYMBOL(z_max96712_stop_streaming);

const struct of_device_id max96712_of_match[] = {
	{ .compatible = "ruge,r_max96712", },
	{ },
};
MODULE_DEVICE_TABLE(of, max96712_of_match);

static int anomaly_detection(void *data) 
{
	struct max96712 *priv = (struct max96712 *)data;
	struct i2c_client *client = priv->i2c_client;
	uint32_t val = 0;
	
	while (!kthread_should_stop())
	//这个判断条件的值从何而来
	//线程在驱动初始化（probe）时被启动，一直后台循环运行；
	//直到驱动卸载（remove）时，才被主动停止并结束。
	{
		mutex_lock(&priv->lock);
		// 只监测启动的link
		if (priv->des_link_start_stream[0]) 
		//内核线程专用循环
		//意思：只要没人停止线程，就一直循环
		{
			dev_info(&client->dev,"anomaly_detection: check link_A\n");	
			max96712_read_reg(&client->dev,0x1a,&val);
			//主要确定功能：指示 Link A 的 GMSL2 双向链路是否成功锁定
			dev_info(&client->dev,"anomaly_detection: 0x1a=0x%02x\n",val);
			//输出到哪
			max96712_read_reg(&client->dev,0x1dc,&val);
			//视频通道是否锁定
			dev_info(&client->dev,"anomaly_detection: 0x1dc=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x35,&val);
			//链路A上的解码错误数量
			dev_info(&client->dev,"anomaly_detection: 0x35=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x39,&val);
			//空闲字错误数量
			dev_info(&client->dev,"anomaly_detection: 0x39=0x%02x\n",val);
		}
		
		if (priv->des_link_start_stream[1]) {
			dev_info(&client->dev,"anomaly_detection: check link_B\n");
			max96712_read_reg(&client->dev,0x0a,&val);
			dev_info(&client->dev,"anomaly_detection: 0x0a=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x1fc,&val);
			dev_info(&client->dev,"anomaly_detection: 0x1fc=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x36,&val);
			dev_info(&client->dev,"anomaly_detection: 0x36=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x3a,&val);
			dev_info(&client->dev,"anomaly_detection: 0x3a=0x%02x\n",val);
		} 
		
		if (priv->des_link_start_stream[2]) {
			dev_info(&client->dev,"anomaly_detection: check link_C\n");
			max96712_read_reg(&client->dev,0x0b,&val);
			dev_info(&client->dev,"anomaly_detection: 0x0b=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x21c,&val);
			dev_info(&client->dev,"anomaly_detection: 0x21c=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x37,&val);
			dev_info(&client->dev,"anomaly_detection: 0x37=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x3b,&val);
			dev_info(&client->dev,"anomaly_detection: 0x3b=0x%02x\n",val);
		} 
		
		if (priv->des_link_start_stream[3]) {
			dev_info(&client->dev,"anomaly_detection: check link_D\n");
			max96712_read_reg(&client->dev,0x0c,&val);
			dev_info(&client->dev,"anomaly_detection: 0x0c=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x23c,&val);
			dev_info(&client->dev,"anomaly_detection: 0x23c=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x38,&val);
			dev_info(&client->dev,"anomaly_detection: 0x38=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x3c,&val);
			dev_info(&client->dev,"anomaly_detection: 0x3c=0x%02x\n",val);
		}
			
		if (priv->des_link_start_stream[3] || priv->des_link_start_stream[2] || priv->des_link_start_stream[1] || priv->des_link_start_stream[0]) {
			// 只要有link启动，就检查all_err	
			dev_info(&client->dev,"anomaly_detection: check streaming err\n");
			max96712_read_reg(&client->dev,0x26,&val);
			//报告 A/B/C/D 四条 GMSL 链路的解码错误计数是否超过了预设阈值 DEC_ERR_THR。
			//每个链路对应一个状态位，只读，用来判断链路的信号质量是否恶化。
			dev_info(&client->dev,"anomaly_detection: 0x26=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x28,&val);
			//四路链路的眼图裕度（Eye-opening）是否低于阈值（链路信号质量）
			//保留内存恢复过程中的 CRC 错误
			//两个水印检测器（Watermark）的错误状态
			dev_info(&client->dev,"anomaly_detection: 0x28=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x2a,&val);
			//本质：它不是某一个单一错误，而是 A/B/C/D 四路链路 以及 视频线 CRC、远程侧错误、帧同步错误 的 “大杂烩” 状态寄存器。
			dev_info(&client->dev,"anomaly_detection: 0x2a=0x%02x\n",val);
			max96712_read_reg(&client->dev,0x2c,&val);
			//用来监控 4 路 GMSL 链路的 Idle Word（空闲字）错误计数是否超过预设阈值。
			dev_info(&client->dev,"anomaly_detection: 0x2c=0x%02x\n",val);
		}



		mutex_unlock(&priv->lock);
		set_current_state(TASK_INTERRUPTIBLE);
		//设置为可中断休眠
    	schedule_timeout(msecs_to_jiffies(1000));
	}

	dev_info(&client->dev,"anomaly_detection: thread stopping\n");
	return 0;
}

static int max96712_parse_dt(struct max96712 *priv,
				struct i2c_client *client)
{
	struct device_node *node = client->dev.of_node;
	const struct of_device_id *match;
	//match：用来保存设备树与驱动的匹配结果
	if (!node)
	//检测出该设备树节点不为空
		return -EINVAL;

	match = of_match_device(max96712_of_match, &client->dev);
	if (!match) {
		dev_err(&client->dev, "Failed to find matching dt id\n");
		return -EFAULT;
	}

	return 0;
}

static struct regmap_config max96712_regmap_config = {
	.reg_bits = 16,
	//寄存器地址范围：0x0000 ~ 0xFFFF
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static int max96712_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct max96712 *priv;
	int err = 0;
	u32 val = 0;
	struct device_node *node = client->dev.of_node;
	//传递的参数结构体里面的真实数据从何而来

	dev_info(&client->dev, "zuojisi: [MAX96712]: probing GMSL Deserializer @0x%x\n",client->addr);
	//输出设备地址

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	//在内核申请内存，存放 max96712 结构体
	priv->i2c_client = client;
	priv->regmap = devm_regmap_init_i2c(priv->i2c_client,
				&max96712_regmap_config);
				//初始化 regmap（寄存器操作接口）
				//写代码通过iic总线操作寄存器
	if (IS_ERR(priv->regmap)) {
		dev_err(&client->dev,
			"regmap init failed: %ld\n", PTR_ERR(priv->regmap));
		return -ENODEV;
	}

	err = max96712_parse_dt(priv, client);
	//驱动和设备树的匹配，核心是比对 compatible 属性字符串，全程在内存里完成
	if (err) {
		dev_err(&client->dev, "zuojisi: unable to parse dt\n");
		return -EFAULT;
	}

	mutex_init(&priv->lock);

	dev_set_drvdata(&client->dev, priv);
	//把私有数据挂在设备上
	//其他函数可以通过 dev_get_drvdata 取回来

	err=check_device_id_rev(client);
	if(err){
		dev_err(&client->dev, "zuojisi: failed on check id & rev\n");
		return -EFAULT;
	}


	if (of_get_property(node, "is-master", NULL)) {
		priv->ismaster = 1;
		dev_info(&client->dev, "zuojisi: this 96712 is master\n");
	}else{
		priv->ismaster = 0;
		dev_info(&client->dev, "zuojisi: this 96712 is slave\n");
	}
	//多个 MAX96712 解串器分成主从模式
	//Master = 1
	//输出 FSync 帧同步信号
	//提供时钟
	//指挥所有摄像头同步
	//lave = 0
	//输入 FSync，跟随主芯片同步
	//不产生时钟

	if( check_version(client)<0 ){
		return -EFAULT;
	}

	err=max96712_write_reg(&client->dev,0x0013,0x40); //reset all
	mdelay(100);
	err=max96712_write_reg(&client->dev,0x0013,0x00);//释放复位

	//enable VDD LDO Regulator
    err=max96712_write_reg(&client->dev,0x0017,0x04);
	err=max96712_write_reg(&client->dev,0x0019,0x10);
	//VDD LDO Regulator Enable（稳压器使能）
	//Works in conjunction with bitfield REG_MNL
	//(0x19). Enable function is available when
	//REG_MNL = 1. If REG_MNL = 0, manual
	//control using REG_ENABLE is not available.
	//VDD=1.2v

	//turn on LED
	//err=max96712_write_reg(&client->dev,0x0300,0x90);

	//disable Links which are not Locked
	err=max96712_read_reg(&client->dev,0x0006,&val);
	//写入的值从何而来
	err=max96712_write_reg(&client->dev,0x0006,(val&0xF0)|(priv->link_lock_status&0x0F));
	//链路使能

	// set GMSL 3Gbps
	err=max96712_write_reg(&client->dev,0x0010,0x11);
	err=max96712_write_reg(&client->dev,0x0011,0x11);

	max96712_write_reg(&client->dev,0x94a,0xc0); //4 lanes, D-Phy, 2bits VC
	max96712_write_reg(&client->dev,0x98a,0xc0); //4 lanes, D-Phy, 2bits VC

	max96712_write_reg(&client->dev,0x8a3,0xe4); //phy lane mapping
	max96712_write_reg(&client->dev,0x8a4,0xe4); //phy lane mapping

    max96712_write_reg(&client->dev,0x8a0,0x04); // 2x4 mode
	max96712_write_reg(&client->dev,0x8A9,0xC8); // phy copy 

	max96712_write_reg(&client->dev,0x415,MAX96712_PHY_CLK); 
	max96712_write_reg(&client->dev,0x418,MAX96712_PHY_CLK);
	max96712_write_reg(&client->dev,0x41b,MAX96712_PHY_CLK);
	max96712_write_reg(&client->dev,0x41e,MAX96712_PHY_CLK);

	max96712_write_reg(&client->dev,0xf0,0x62); //pipe1 GMSLB, Z-pipe; pipe0 GMSLA, Z-pipe
	max96712_write_reg(&client->dev,0xf1,0xea); //pipe3 GMSLD, Z-pipe; pipe2 GMSLC, Z-pipe
	max96712_write_reg(&client->dev,0xf4,0x00); //disable pipes 0-4 (default val)

	max96712_write_reg(&client->dev,0x0023,0x0f); //auto reset dec error

	// mfp2 FSync
	max96712_write_reg(&client->dev,0x0306,0x82);
	max96712_write_reg(&client->dev,0x033d,0x22);
	max96712_write_reg(&client->dev,0x0374,0x22);
	max96712_write_reg(&client->dev,0x03aa,0x22);

	max96712_write_reg(&client->dev,0x0027,0x00); //disable unconcerned error report
	max96712_write_reg(&client->dev,0x0029,0xf0); //disable unconcerned error report
	max96712_write_reg(&client->dev,0x1250,0xff); //set 128 ECC threshold, reset error cnt

	/* dev communication gets validated when GMSL link setup is done */
	dev_info(&client->dev, "zuojisi: %s:  success\n", __func__);

	priv->anomaly_thread = kthread_run(anomaly_detection, priv, "max96712_anomaly");

	return err;
}

static int check_device_id_rev(struct i2c_client *client)
//读设备id
{
	int err=0;
	unsigned int value=0;

	err=max96712_read_reg(&client->dev,0x004C,&value);
	dev_info(&client->dev,"zuojisi:device revision=0x%x\n",(value&0x0f));
	return err;
}

static int check_version(struct i2c_client *client)
{
	struct max96712 *priv = NULL;
	const uint16_t ver0addr = 0x02B0; //ver0 : mfp0
	const uint16_t ver1addr = 0x02BF; //ver1 : mfp5
	const uint16_t ver2addr = 0x02C5; //ver2 : mfp7
	const uint16_t ver3addr = 0x02C8; //ver3 : mfp8

	uint16_t veraddrs[4] = {ver0addr,ver1addr,ver2addr,ver3addr};
	uint32_t ver=0,tmp;
	int i=0,err=0;

	priv = dev_get_drvdata(&client->dev);
	CHECKNULL(priv);

	if(priv->ismaster){
		//disable output
		for(i=0;i<4;i++){
			err=max96712_write_reg(&client->dev,veraddrs[i],0x85);
		}
		usleep_range(500, 1000);
		for(i=0;i<4;i++){
			err=max96712_read_reg(&client->dev,veraddrs[i],&tmp);
			tmp = (tmp & 0x8)>>3;
			ver = ver | (tmp<<i);
		}
		dev_info(&client->dev,"zuojisi: version=%d\n",ver);
		if(ver==0){
			vercheck=1;
		}else{
			vercheck=-1;
			err=-1;
		}
	}else{
		if(vercheck<0){
			err=-1;
		}
	}

	return err;
}

static int max96712_remove(struct i2c_client *client)
{
	struct max96712 *priv;
	uint16_t i2cdev = client->addr;

	if (client != NULL) {
		/* 执行设备复位操作 */
		max96712_write_reg(&client->dev, 0x13, 0x40); // Reset all
		dev_info(&client->dev,"0x%X: max96712: device reset done\n", i2cdev);

		/* 获取设备的私有数据 */
		priv = dev_get_drvdata(&client->dev);
		
		/* 确保没有其他线程持有锁 */
		mutex_lock(&priv->lock);
		dev_info(&client->dev,"0x%X: max96712: device lock acquired for removal\n", i2cdev);
		
		/* 销毁互斥锁 */
		mutex_destroy(&priv->lock);
		dev_info(&client->dev,"0x%X: max96712: mutex destroyed\n", i2cdev);
		
		/* 清除设备的驱动数据 */
		dev_set_drvdata(&client->dev, NULL);

		/* 停止异常检测线程 */
		if (priv->anomaly_thread) {
			kthread_stop(priv->anomaly_thread);
		}
	}
	printk("0x%X: max96712: device removed successfully\n", i2cdev);
	return 0;
}

static const struct i2c_device_id max96712_id[] = {
	{ "max96712", 0 },
	{ },
};

MODULE_DEVICE_TABLE(i2c, max96712_id);

static struct i2c_driver max96712_i2c_driver = {
	.driver = {
		.name = "z_max96712",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(max96712_of_match),
	},
	.probe = max96712_probe,
	.remove = max96712_remove,
	.id_table = max96712_id,
};

static int __init max96712_init(void)
{
	return i2c_add_driver(&max96712_i2c_driver);
}

static void __exit max96712_exit(void)
{
	i2c_del_driver(&max96712_i2c_driver);
}

module_init(max96712_init);
module_exit(max96712_exit);

MODULE_DESCRIPTION("GMSL Deserializer driver for r_max96712");
MODULE_AUTHOR("ruge.cool@qq.com");
MODULE_LICENSE("GPL v2");
