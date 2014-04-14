/*
 * Copyright (C) 2005-2006 Micronas USA Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (Version 2) as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/unistd.h>
#include <linux/time.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#include "go7007-priv.h"
#include "wis-i2c.h"



#define SPI_I2C_ADDR_BASE		0x1400
#define STATUS_REG_ADDR			(SPI_I2C_ADDR_BASE + 0x2)
#define I2C_CTRL_REG_ADDR		(SPI_I2C_ADDR_BASE + 0x6)
#define I2C_DEV_UP_ADDR_REG_ADDR	(SPI_I2C_ADDR_BASE + 0x7)
#define I2C_LO_ADDR_REG_ADDR		(SPI_I2C_ADDR_BASE + 0x8)
#define I2C_DATA_REG_ADDR		(SPI_I2C_ADDR_BASE + 0x9)
#define I2C_CLKFREQ_REG_ADDR		(SPI_I2C_ADDR_BASE + 0xa)

#define I2C_STATE_MASK			0x0007
#define I2C_READ_READY_MASK		0x0008

static DEFINE_MUTEX(adlink_mpg24_i2c_lock);

static int go7007_i2c_xfer(struct go7007 *go, u16 addr, int read,
		u16 command, int flags, u8 *data)
{
	int i, ret = -1;
	u16 val;

	if (go->status == STATUS_SHUTDOWN)
		return -1;

#ifdef GO7007_I2C_DEBUG
	if (read)
		printk(KERN_DEBUG "go7007-i2c: reading 0x%02x on 0x%02x\n",
			command, addr);
	else
		printk(KERN_DEBUG
			"go7007-i2c: writing 0x%02x to 0x%02x on 0x%02x\n",
			*data, command, addr);
#endif

	mutex_lock(&go->hw_lock);

	if (go->board_id == GO7007_BOARDID_ADLINK_MPG24) {
		
		mutex_lock(&adlink_mpg24_i2c_lock);
		go7007_write_addr(go, 0x3c82, 0x0020);
	}

	
	for (i = 0; i < 10; ++i) {
		if (go7007_read_addr(go, STATUS_REG_ADDR, &val) < 0)
			goto i2c_done;
		if (!(val & I2C_STATE_MASK))
			break;
		msleep(100);
	}
	if (i == 10) {
		printk(KERN_ERR "go7007-i2c: I2C adapter is hung\n");
		goto i2c_done;
	}

	
	go7007_write_addr(go, I2C_CTRL_REG_ADDR, flags);
	go7007_write_addr(go, I2C_LO_ADDR_REG_ADDR, command);

	
	if (!read) {
		go7007_write_addr(go, I2C_DATA_REG_ADDR, *data);
		go7007_write_addr(go, I2C_DEV_UP_ADDR_REG_ADDR,
					(addr << 9) | (command >> 8));
		ret = 0;
		goto i2c_done;
	}

	
	if (go7007_read_addr(go, I2C_DATA_REG_ADDR, &val) < 0)
		goto i2c_done;

	
	go7007_write_addr(go, I2C_DEV_UP_ADDR_REG_ADDR,
			(addr << 9) | 0x0100 | (command >> 8));

	
	for (i = 0; i < 10; ++i) {
		if (go7007_read_addr(go, STATUS_REG_ADDR, &val) < 0)
			goto i2c_done;
		if (val & I2C_READ_READY_MASK)
			break;
		msleep(100);
	}
	if (i == 10) {
		printk(KERN_ERR "go7007-i2c: I2C adapter is hung\n");
		goto i2c_done;
	}

	
	if (go7007_read_addr(go, I2C_DATA_REG_ADDR, &val) < 0)
		goto i2c_done;
	*data = val;
	ret = 0;

i2c_done:
	if (go->board_id == GO7007_BOARDID_ADLINK_MPG24) {
		
		go7007_write_addr(go, 0x3c82, 0x0000);
		mutex_unlock(&adlink_mpg24_i2c_lock);
	}
	mutex_unlock(&go->hw_lock);
	return ret;
}

static int go7007_smbus_xfer(struct i2c_adapter *adapter, u16 addr,
		unsigned short flags, char read_write,
		u8 command, int size, union i2c_smbus_data *data)
{
	struct go7007 *go = i2c_get_adapdata(adapter);

	if (size != I2C_SMBUS_BYTE_DATA)
		return -1;
	return go7007_i2c_xfer(go, addr, read_write == I2C_SMBUS_READ, command,
			flags & I2C_CLIENT_SCCB ? 0x10 : 0x00, &data->byte);
}


static int go7007_i2c_master_xfer(struct i2c_adapter *adapter,
					struct i2c_msg msgs[], int num)
{
	struct go7007 *go = i2c_get_adapdata(adapter);
	int i;

	for (i = 0; i < num; ++i) {
		if (msgs[i].len == 2) {
			if (i + 1 == num || msgs[i].addr != msgs[i + 1].addr ||
					(msgs[i].flags & I2C_M_RD) ||
					!(msgs[i + 1].flags & I2C_M_RD) ||
					msgs[i + 1].len != 1)
				return -1;
			if (go7007_i2c_xfer(go, msgs[i].addr, 1,
					(msgs[i].buf[0] << 8) | msgs[i].buf[1],
					0x01, &msgs[i + 1].buf[0]) < 0)
				return -1;
			++i;
		} else if (msgs[i].len == 3) {
			if (msgs[i].flags & I2C_M_RD)
				return -1;
			if (msgs[i].len != 3)
				return -1;
			if (go7007_i2c_xfer(go, msgs[i].addr, 0,
					(msgs[i].buf[0] << 8) | msgs[i].buf[1],
					0x01, &msgs[i].buf[2]) < 0)
				return -1;
		} else
			return -1;
	}

	return 0;
}

static u32 go7007_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_BYTE_DATA;
}

static struct i2c_algorithm go7007_algo = {
	.smbus_xfer	= go7007_smbus_xfer,
	.master_xfer	= go7007_i2c_master_xfer,
	.functionality	= go7007_functionality,
};

static struct i2c_adapter go7007_adap_templ = {
	.owner			= THIS_MODULE,
	.name			= "WIS GO7007SB",
	.algo			= &go7007_algo,
};

int go7007_i2c_init(struct go7007 *go)
{
	memcpy(&go->i2c_adapter, &go7007_adap_templ,
			sizeof(go7007_adap_templ));
	go->i2c_adapter.dev.parent = go->dev;
	i2c_set_adapdata(&go->i2c_adapter, go);
	if (i2c_add_adapter(&go->i2c_adapter) < 0) {
		printk(KERN_ERR
			"go7007-i2c: error: i2c_add_adapter failed\n");
		return -1;
	}
	return 0;
}
