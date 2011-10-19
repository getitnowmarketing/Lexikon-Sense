/* drivers/input/touchscreen/synaptics_3k.c - Synaptics 3k serious touch panel driver
 *
 * Copyright (C) 2010 HTC Corporation.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/synaptics_i2c_rmi.h>
#include <linux/slab.h>
#include <mach/board.h>
#include <mach/msm_hsusb.h>

#define SYN_I2C_RETRY_TIMES 10

struct synaptics_ts_data {
	uint16_t addr;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct workqueue_struct *syn_wq;
	struct function_t *address_table;
	int use_irq;
	struct hrtimer timer;
	struct work_struct  work;
	uint16_t max[2];
	uint32_t flags;
	uint8_t num_funtion;
	int8_t sensitivity_adjust;
	uint8_t finger_support;
	uint16_t finger_pressed;
	int (*power)(int on);
	struct early_suspend early_suspend;
	int pre_finger_data[11][2];
	uint8_t debug_log_level;
	uint32_t raw_base;
	uint32_t raw_ref;
	uint64_t timestamp;
	uint16_t *filter_level;
	uint8_t grip_suppression;
	uint8_t grip_b_suppression;
	uint8_t ambiguous_state;
	uint8_t diag_command;
	uint8_t cable_support;
	uint8_t cable_config;
	uint8_t key_number;
	uint16_t key_postion_x[4];
	uint16_t key_postion_y;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h);
static void synaptics_ts_late_resume(struct early_suspend *h);
#endif

static struct synaptics_ts_data *gl_ts;
static const char SYNAPTICSNAME[]	= "Synaptics_3K";
static uint32_t syn_panel_version;

int i2c_syn_read(struct i2c_client *client, uint8_t addr, uint8_t *data, uint8_t length)
{
	int retry;

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 2) == 2)
			break;
		msleep(10);
	}
	if (retry == SYN_I2C_RETRY_TIMES) {
		printk(KERN_INFO "i2c_read_block retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}
	return 0;

}

int i2c_syn_write(struct i2c_client *client, uint8_t addr, uint8_t *data, uint8_t length)
{
	int retry, loop_i;
	uint8_t buf[length + 1];

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	buf[0] = addr;
	for (loop_i = 0; loop_i < length; loop_i++)
		buf[loop_i + 1] = data[loop_i];

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1)
			break;
		mdelay(10);
	}

	if (retry == SYN_I2C_RETRY_TIMES) {
		printk(KERN_ERR "i2c_write_block retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}
	return 0;

}

int i2c_syn_write_byte_data(struct i2c_client *client, uint8_t addr, uint8_t value)
{
	return i2c_syn_write(client, addr, &value, 1);
}

int get_address_base(struct synaptics_ts_data *ts, uint8_t command, uint8_t type)
{
	uint8_t loop_i;
	for (loop_i = 0; loop_i < ts->num_funtion; loop_i++) {
		if (ts->address_table[loop_i].function_type == command) {
			switch (type) {
			case QUERY_BASE:
				return ts->address_table[loop_i].query_base;
			case COMMAND_BASE:
				return ts->address_table[loop_i].command_base;
			case CONTROL_BASE:
				return ts->address_table[loop_i].control_base;
			case DATA_BASE:
				return ts->address_table[loop_i].data_base;
			case INTR_SOURCE:
				return ts->address_table[loop_i].interrupt_source;
			case FUNCTION:
				return 1;
			}
		}
	}
	if (type == FUNCTION)
		return 0;
	else
		return -1;
}

static ssize_t touch_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;

	sprintf(buf, "%s_%#x\n", SYNAPTICSNAME, syn_panel_version);
	ret = strlen(buf) + 1;

	return ret;
}

static DEVICE_ATTR(vendor, 0444, touch_vendor_show, NULL);

static uint16_t syn_reg_addr;

static ssize_t register_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	uint8_t data;
	struct synaptics_ts_data *ts;
	ts = gl_ts;

	data = i2c_smbus_read_byte_data(ts->client, syn_reg_addr);
	if (data < 0) {
		printk(KERN_WARNING "%s: read fail\n", __func__);
		return ret;
	}
	ret += sprintf(buf, "addr: 0x%X, data: 0x%X\n", syn_reg_addr, data);
	return ret;
}

static ssize_t register_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	struct synaptics_ts_data *ts;
	char buf_tmp[4];
	uint8_t write_da;

	ts = gl_ts;
	memset(buf_tmp, 0x0, sizeof(buf_tmp));
	if ((buf[0] == 'r' || buf[0] == 'w') && buf[1] == ':' &&
		(buf[5] == ':' || buf[5] == '\n')) {
		memcpy(buf_tmp, buf + 2, 3);
		syn_reg_addr = simple_strtol(buf_tmp, NULL, 16);
		printk(KERN_DEBUG "%s: set syn_reg_addr is: 0x%X\n",
						__func__, syn_reg_addr);
		if (buf[0] == 'w' && buf[5] == ':' && buf[9] == '\n') {
			memcpy(buf_tmp, buf + 6, 3);
			write_da = simple_strtol(buf_tmp, NULL, 10);
			printk(KERN_DEBUG "write addr: 0x%X, data: 0x%X\n",
						syn_reg_addr, write_da);
			ret = i2c_smbus_write_byte_data(ts->client,
					syn_reg_addr, write_da);
			if (ret < 0) {
				printk(KERN_ERR "%s: write fail(%d)\n",
								__func__, ret);
			}
		}
	}

	return count;
}

static DEVICE_ATTR(register, 0644, register_show, register_store);

static ssize_t debug_level_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;

	return sprintf(buf, "%d\n", ts->debug_log_level);
}

static ssize_t debug_level_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;

	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		ts->debug_log_level = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(debug_level, 0644, debug_level_show, debug_level_store);

static ssize_t syn_diag_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts_data;
	size_t count = 0;
	uint8_t data[5], x, y;
	uint16_t loop_i;
	int16_t rawdata;
	ts_data = gl_ts;
	memset(data, 0x0, sizeof(data));

	i2c_syn_read(ts_data->client,
		get_address_base(ts_data, 0x54, QUERY_BASE), data, 2);
	y = data[0];
	x = data[1];

	count += sprintf(buf, "Channel: %d * %d\n", x, y);

	for (loop_i = 0; loop_i < x*y*2 + (ts_data->key_number * 2); loop_i += 2) {
		data[0] = ts_data->diag_command;
		data[1] = loop_i % 256;
		data[2] = loop_i / 256;
		i2c_syn_write(ts_data->client,
			get_address_base(ts_data, 0x54, DATA_BASE), data, 3);
		i2c_syn_read(ts_data->client,
			get_address_base(ts_data, 0x54, DATA_BASE) + 3, &data[3], 2);
		if (loop_i == x*y*2)
			count += sprintf(buf + count, "\n0D:\n");
		rawdata = data[4] << 8 | data[3];
		count += sprintf(buf + count, "%5d", rawdata);
		if (((loop_i + 1) % (2 * y)) == (2 * y - 1))
			count += sprintf(buf + count, "\n");
	}

	return count;
}

static ssize_t syn_diag_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts_data;
	ts_data = gl_ts;
	if (buf[0] == '1')
		ts_data->diag_command = 2;
	if (buf[0] == '2')
		ts_data->diag_command = 3;

	return count;
}

static DEVICE_ATTR(diag, (S_IWUSR|S_IRUGO),
	syn_diag_show, syn_diag_dump);


static struct kobject *android_touch_kobj;

static int synaptics_touch_sysfs_init(void)
{
	int ret;
	android_touch_kobj = kobject_create_and_add("android_touch", NULL);
	if (android_touch_kobj == NULL) {
		printk(KERN_ERR "%s: subsystem_register failed\n", __func__);
		ret = -ENOMEM;
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_vendor.attr);
	if (ret) {
		printk(KERN_ERR "touch_sysfs_init: sysfs_create_group failed\n");
		return ret;
	}
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_debug_level.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
	if (get_address_base(gl_ts, 0x54, FUNCTION)) {
		ret = sysfs_create_file(android_touch_kobj, &dev_attr_diag.attr);
		if (ret) {
			printk(KERN_ERR "TOUCH_ERR: create_file diag failed\n");
			return ret;
		}
	}
	syn_reg_addr = 0;
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_register.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}

	return 0;
}

static void synaptics_touch_sysfs_remove(void)
{
	sysfs_remove_file(android_touch_kobj, &dev_attr_vendor.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_debug_level.attr);
	if (get_address_base(gl_ts, 0x54, FUNCTION))
		sysfs_remove_file(android_touch_kobj, &dev_attr_diag.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_register.attr);
	kobject_del(android_touch_kobj);
}

static int synaptics_init_panel(struct synaptics_ts_data *ts)
{
	int ret = 0;
	uint8_t data[2];
	if (ts->sensitivity_adjust) {
		ret = i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x11, CONTROL_BASE) + 0x48,
			ts->sensitivity_adjust); /* Set Sensitivity */
		if (ret < 0)
			printk(KERN_ERR "TOUCH_ERR: i2c_smbus_write_byte_data failed for Sensitivity Set\n");
	}

	/* Position Threshold */
	i2c_syn_write_byte_data(ts->client, get_address_base(ts, 0x11, CONTROL_BASE), 0x01);
	data[0] = 3;
	data[1] = 3;
	i2c_syn_write(ts->client, get_address_base(ts, 0x11, CONTROL_BASE) + 2, data, 2);
	i2c_syn_read(ts->client, get_address_base(ts, 0x11, CONTROL_BASE) + 2, data, 2);

	/* 2D Gesture Enable */
	i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x11, CONTROL_BASE) + 10, 0);
	i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x11, CONTROL_BASE) + 11, 0);

	/* Configured */
	if (ts->cable_support)
		i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x01, CONTROL_BASE), ts->cable_config | 0x80);
	else
		i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x01, CONTROL_BASE), 0x80);

	return ret;

}

static void synaptics_ts_work_func(struct work_struct *work)
{
	int i;
	int ret;

	struct synaptics_ts_data *ts = container_of(work, struct synaptics_ts_data, work);
	uint8_t buf[((ts->finger_support * 21 + 15) / 4) +
			get_address_base(ts, 0x19, FUNCTION)];
	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, DATA_BASE), buf, sizeof(buf));
	if (ret < 0 || ((buf[0] & 0x0F))) {
		if (ret < 0)
			printk(KERN_ERR "TOUCH_ERR: synaptics_ts_work_func: i2c_transfer failed\n");
		else
			printk(KERN_INFO "TOUCH_ERR: synaptics_ts_work_func: Status ERROR: %d\n", buf[0] & 0x0F);
		/* reset touch control */
		if (ts->power) {
			ret = ts->power(0);
			if (ret < 0)
				printk(KERN_ERR "TOUCH_ERR: synaptics_ts_work_func power off failed\n");
			msleep(10);
			ret = ts->power(1);
			if (ret < 0)
				printk(KERN_ERR "synaptics_ts_work_func power on failed\n");
		} else {
			i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x01, COMMAND_BASE), 0x01);
			msleep(250);
		}
		synaptics_init_panel(ts);
		if (!ts->use_irq)
			hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		else {
			uint8_t intr_bit = (get_address_base(ts, 0x11, FUNCTION) ?
				BIT(get_address_base(ts, 0x11, INTR_SOURCE)) : 0) |
				(get_address_base(ts, 0x19, FUNCTION) ?
				BIT(get_address_base(ts, 0x19, INTR_SOURCE)) : 0);
			i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x01, CONTROL_BASE) + 1, intr_bit);
		}
	} else {
		if (ts->debug_log_level & 0x1) {
			printk("Touch:");
			for (i = 0; i < sizeof(buf); i++)
				printk(" %2x", buf[i]);
			printk("\n");
		}
		if (get_address_base(ts, 0x19, FUNCTION) &&
			(buf[1] & BIT(get_address_base(ts, 0x19, INTR_SOURCE)))) {
			for (i = 0; i < 4; i++) {
				if (((buf[sizeof(buf) - 1] >> i) & 0x01) == 0x01) {
					input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 10 << 16 | 10);
					input_report_abs(ts->input_dev, ABS_MT_POSITION,
						1 << 31 | ts->key_postion_x[i] << 16 | ts->key_postion_y);
					break;
				}
			}
			if (i >= 4) {
				input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
				input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
			}
		} else {
		int finger_data[ts->finger_support][4];
		int base = (ts->finger_support + 11) / 4;
		uint8_t loop_i, loop_j, finger_pressed = 0, finger_count = 0;
		uint8_t finger_press_changed = 0, finger_release_changed = 0;
		uint8_t pos_mask = 0x0f;

		for (loop_i = 0; loop_i < ts->finger_support; loop_i++) {
			if (buf[1 + (ts->finger_support + 3) / 4] >> (loop_i * 2) & 0x03) {
				finger_pressed |= 1 << loop_i;
				finger_count++;
			} else if (ts->grip_suppression & BIT(loop_i)) {
				ts->grip_suppression &= ~BIT(loop_i);
				ts->grip_b_suppression &= ~BIT(loop_i);
			}
		}
		if (ts->finger_pressed != finger_pressed
			&& (ts->pre_finger_data[0][0] < 2 || ts->filter_level[0])) {
			finger_press_changed = ts->finger_pressed ^ finger_pressed;
			finger_release_changed = finger_press_changed & (~finger_pressed);
			finger_press_changed &= finger_pressed;
			ts->finger_pressed = finger_pressed;
		}
		if (finger_pressed == 0 ||
			((ts->grip_suppression | ts->grip_b_suppression) == finger_pressed && finger_release_changed)) {
#ifdef CONFIG_TOUCHSCREEN_COMPATIBLE_REPORT
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
#else
			input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
			input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
#endif
			ts->ambiguous_state = 0;
			ts->grip_b_suppression = 0;

			if (ts->debug_log_level & 0x2)
				printk(KERN_INFO "Finger leave\n");
		}
		if (ts->pre_finger_data[0][0] < 2 || finger_pressed) {
		for (loop_i = 0; loop_i < ts->finger_support; loop_i++) {
			uint32_t flip_flag = SYNAPTICS_FLIP_X;
			if ((((finger_pressed | finger_release_changed) >> loop_i) & 1) == 1) {
				pos_mask = 0x0f;
				for (loop_j = 0; loop_j < 2; loop_j++) {
					finger_data[loop_i][loop_j]
						= (buf[base+2] & pos_mask) >> (loop_j * 4) |
						(uint16_t)buf[base + loop_j] << 4;
					if (ts->flags & flip_flag)
						finger_data[loop_i][loop_j]
							= ts->max[loop_j] - finger_data[loop_i][loop_j];
					flip_flag <<= 1;
					pos_mask <<= 4;
				}
				finger_data[loop_i][2] = (buf[base+3] >> 4 & 0x0F) + (buf[base+3] & 0x0F);
				if (((buf[base+3] >> 4 & 0x0F) - (buf[base+3] & 0x0F)) >= 10)
					ts->grip_b_suppression |= BIT(loop_i);
				finger_data[loop_i][3] = buf[base+4];
				if (ts->flags & SYNAPTICS_SWAP_XY)
					swap(finger_data[loop_i][0], finger_data[loop_i][1]);
				if (((finger_release_changed >> loop_i) & 0x1) && ts->pre_finger_data[0][0] < 2) {
					printk(KERN_INFO "E%d@%d, %d\n", loop_i + 1,
					finger_data[loop_i][0], finger_data[loop_i][1]);
				}

				if (ts->filter_level[0] &&
					((finger_press_changed | ts->grip_suppression) & BIT(loop_i))) {
					if ((finger_data[loop_i][0] < (ts->filter_level[0] + ts->ambiguous_state * 20) ||
						finger_data[loop_i][0] > (ts->filter_level[3] - ts->ambiguous_state * 20)) &&
						!(ts->grip_suppression & BIT(loop_i))) {
						ts->grip_suppression |= BIT(loop_i);
					} else if ((finger_data[loop_i][0] < (ts->filter_level[1] + ts->ambiguous_state * 20) ||
						finger_data[loop_i][0] > (ts->filter_level[2] - ts->ambiguous_state * 20)) &&
						(ts->grip_suppression & BIT(loop_i)))
						ts->grip_suppression |= BIT(loop_i);
					else if (finger_data[loop_i][0] > (ts->filter_level[1] + ts->ambiguous_state * 20) &&
						finger_data[loop_i][0] < (ts->filter_level[2] - ts->ambiguous_state * 20)) {
						ts->grip_suppression &= ~BIT(loop_i);
					}
				}
				if ((ts->grip_suppression | ts->grip_b_suppression) & BIT(loop_i)) {
					finger_pressed &= ~(1 << loop_i);

				} else if (((finger_pressed >> loop_i) & 1) == 1) {
					finger_pressed &= ~(1 << loop_i);
#ifdef CONFIG_TOUCHSCREEN_COMPATIBLE_REPORT
					input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, loop_i);
					input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
						finger_data[loop_i][3]);
					input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
						finger_data[loop_i][2]);
					input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
						finger_data[loop_i][0]);
					input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
						finger_data[loop_i][1]);
					input_mt_sync(ts->input_dev);
#else
					input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, loop_i);
					input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE,
						finger_data[loop_i][3] << 16 | finger_data[loop_i][2]);
					input_report_abs(ts->input_dev, ABS_MT_POSITION,
						(finger_pressed == 0) << 31 |
						finger_data[loop_i][0] << 16 | finger_data[loop_i][1]);
#endif
					if (ts->pre_finger_data[0][0] < 2) {
						if ((finger_press_changed >> loop_i) & 0x1) {
							ts->pre_finger_data[loop_i + 1][0] = finger_data[loop_i][0];
							ts->pre_finger_data[loop_i + 1][1] = finger_data[loop_i][1];
							printk(KERN_INFO "S%d@%d, %d\n", loop_i + 1,
								ts->pre_finger_data[loop_i + 1][0], ts->pre_finger_data[loop_i + 1][1]);
							if (finger_count == ts->finger_support)
								i2c_syn_write_byte_data(ts->client,
										get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
							else if (!ts->pre_finger_data[0][0] && finger_count > 1)
								ts->pre_finger_data[0][0] = 1;
						}
					}
					if (ts->debug_log_level & 0x2)
						printk(KERN_INFO "Finger %d=> X:%d, Y:%d w:%d, z:%d\n",
							loop_i + 1, finger_data[loop_i][0],
							finger_data[loop_i][1], finger_data[loop_i][2],
							finger_data[loop_i][3]);
				}
				if (((finger_release_changed >> loop_i) & 0x1) && ts->pre_finger_data[0][0] < 2)
					i2c_syn_write_byte_data(ts->client,
							get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
					if (!finger_count && !ts->pre_finger_data[0][0]
						&& (jiffies > (ts->timestamp + 15 * HZ) || (finger_data[loop_i][1] > ts->raw_base
						&& abs(ts->pre_finger_data[loop_i + 1][1] - finger_data[loop_i][1]) > ts->raw_ref))) {
						ts->pre_finger_data[0][0] = 2;
						printk(KERN_INFO "Touch Calibration Confirmed\n");
					} else if (!finger_count)
						ts->pre_finger_data[0][0] = 0;
			}
			base += 5;
		}
		ts->ambiguous_state = 0;
		for (loop_i = 0; loop_i < ts->finger_support; loop_i++)
			if (((ts->grip_suppression >> loop_i) & 1) == 1)
				ts->ambiguous_state++;
		}
		}
	}
#ifdef CONFIG_TOUCHSCREEN_COMPATIBLE_REPORT
	input_sync(ts->input_dev);
#endif

	if (ts->debug_log_level & 0x4)
		printk(KERN_INFO "ts->grip_suppression: %x, ts->ambiguous_state: %x\n",
			ts->grip_suppression, ts->ambiguous_state);

	if (ts->use_irq)
		enable_irq(ts->client->irq);

}

static enum hrtimer_restart synaptics_ts_timer_func(struct hrtimer *timer)
{
	struct synaptics_ts_data *ts = container_of(timer, struct synaptics_ts_data, timer);
	queue_work(ts->syn_wq, &ts->work);
	hrtimer_start(&ts->timer, ktime_set(0, 12500000), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}

static irqreturn_t synaptics_ts_irq_handler(int irq, void *dev_id)
{
	struct synaptics_ts_data *ts = dev_id;
	disable_irq_nosync(ts->client->irq);
	queue_work(ts->syn_wq, &ts->work);
	return IRQ_HANDLED;
}
static void cable_tp_status_handler_func(int connect_status)
{
	struct synaptics_ts_data *ts;
	uint8_t data;

	ts = gl_ts;
	printk(KERN_INFO "Touch: cable change to %d\n", connect_status);

	if (connect_status)
		connect_status = 1;
	i2c_syn_read(ts->client, get_address_base(ts, 0x11, CONTROL_BASE), &data, 1);
	ts->cable_config = (data & 0xDF) | (connect_status << 5);
	printk(KERN_INFO "%s: ts->cable_config: %x\n", __func__, ts->cable_config);
	i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x01, CONTROL_BASE), ts->cable_config);
}
static struct t_usb_status_notifier cable_status_handler = {
	.name = "usb_tp_connected",
	.func = cable_tp_status_handler_func,
};

static int synaptics_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	struct synaptics_ts_data *ts;
	uint8_t loop_i, loop_j, intr_count = 0;
	int ret = 0;
	uint16_t max_x, max_y;
	struct synaptics_i2c_rmi_platform_data *pdata;
	uint32_t panel_version;
	uint8_t data[6];

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		printk(KERN_ERR "synaptics_ts_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (ts == NULL) {
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}
	ts->client = client;
	i2c_set_clientdata(client, ts);
	pdata = client->dev.platform_data;
	if (pdata)
		ts->power = pdata->power;
	if (ts->power)
		ret = ts->power(1);

	ret = i2c_syn_read(ts->client, 0x00EE, data, 1);
	if (ret < 0) {
		printk(KERN_INFO "No Synaptics chip\n");
		goto err_detect_failed;
	}

	for (loop_i = 0x00EE, ts->num_funtion = 0; loop_i >= 0x00A2; loop_i -= 6) {
		if (i2c_smbus_read_byte_data(ts->client, loop_i) == 0)
			break;
		else
			ts->num_funtion++;
	}
	ts->address_table = kzalloc(sizeof(struct function_t) * ts->num_funtion, GFP_KERNEL);

	for (loop_i = 0x00E9, loop_j = 0; loop_j < ts->num_funtion; loop_i -= 6, loop_j++) {
		i2c_syn_read(ts->client, loop_i, data, 6);
		ts->address_table[loop_j].query_base = data[0];
		ts->address_table[loop_j].command_base = data[1];
		ts->address_table[loop_j].control_base = data[2];
		ts->address_table[loop_j].data_base = data[3];
		if (data[4]) {
			ts->address_table[loop_j].interrupt_source = intr_count;
			intr_count += data[4];
		}
		ts->address_table[loop_j].function_type = data[5];
		printk(KERN_INFO
			"Query: %2.2X, Command: %4.4X, Control: %2X, Data: %2X, INTR: %2X, Funtion: %2X\n",
			ts->address_table[loop_j].query_base , ts->address_table[loop_j].command_base,
			ts->address_table[loop_j].control_base, ts->address_table[loop_j].data_base,
			ts->address_table[loop_j].interrupt_source, ts->address_table[loop_j].function_type);
	}

	i2c_syn_read(ts->client, get_address_base(ts, 0x01, QUERY_BASE) + 2, data, 2);
	panel_version = data[0] << 8 | data[1];

	printk(KERN_INFO "%s: panel_version: %x\n", __func__, panel_version);
	syn_panel_version = panel_version;

	if (pdata) {
		while (pdata->version > panel_version) {
			printk(KERN_INFO "synaptics_ts_probe: old tp detected, "
					"panel version = %x\n", panel_version);
			pdata++;
		}
		ts->flags = pdata->flags;
		ts->sensitivity_adjust = pdata->sensitivity_adjust;
		ts->finger_support = pdata->finger_support;
		ts->filter_level = pdata->filter_level;
		ts->cable_support = pdata->cable_support;
	}
	i2c_syn_read(ts->client, get_address_base(ts, 0x11, CONTROL_BASE) + 6, data, 4);
	ts->max[0] = max_x = data[0] | data[1] << 8;
	ts->max[1] = max_y = data[2] | data[3] << 8;

	printk(KERN_INFO"max_x: %X, max_y: %X\n", max_x, max_y);

	if (pdata->abs_x_min == pdata->abs_x_max && pdata->abs_y_min == pdata->abs_y_max) {
		pdata->abs_x_min = 0;
		pdata->abs_x_max = max_x;
		pdata->abs_y_min = 0;
		pdata->abs_y_max = max_y;
	}
	if (get_address_base(ts, 0x19, FUNCTION)) {
		i2c_syn_read(ts->client, get_address_base(ts, 0x19, QUERY_BASE) + 1, data, 1);
		ts->key_number = data[0];
		for (loop_i = 0; loop_i < ts->key_number; loop_i++) {
			ts->key_postion_x[loop_i] =
				(pdata->abs_x_max - pdata->abs_x_min) * (loop_i * 2 + 1) / (data[0] * 2)
				+ pdata->abs_x_min;
			printk(KERN_INFO "ts->key_postion_x[%d]: %d\n",
				loop_i, ts->key_postion_x[loop_i]);
		}
		ts->key_postion_y = pdata->abs_y_min +
			(21 * (pdata->abs_y_max - pdata->abs_y_min) / 20);
		printk(KERN_INFO "ts->key_postion_y: %d\n", ts->key_postion_y);
	}
	if (pdata->display_height) {
		ts->raw_ref = 115 * ((pdata->abs_y_max - pdata->abs_y_min) +
			pdata->abs_y_min) / pdata->display_height;
		ts->raw_base = 650 * ((pdata->abs_y_max - pdata->abs_y_min) +
			pdata->abs_y_min) / pdata->display_height;
		printk(KERN_INFO "ts->raw_ref: %d, ts->raw_base: %d\n", ts->raw_ref, ts->raw_base);
	} else {
		ts->raw_ref = 0;
		ts->raw_base = 0;
	}

	if (ts->cable_support) {
		i2c_syn_read(ts->client,
			get_address_base(ts, 0x11, CONTROL_BASE), &ts->cable_config, 1);
		if (usb_get_connect_type())
			ts->cable_config = ((ts->cable_config & 0xDF) | (1 << 5));
		printk(KERN_INFO "%s: ts->cable_config: %x\n", __func__, ts->cable_config);
	}
	ret = synaptics_init_panel(ts);

	ts->timestamp = jiffies + 60 * HZ;

	ts->syn_wq = create_singlethread_workqueue("synaptics_wq");
	if (!ts->syn_wq)
		goto err_create_wq_failed;
	INIT_WORK(&ts->work, synaptics_ts_work_func);

	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		printk(KERN_ERR "TOUCH_ERR: synaptics_ts_probe: Failed to allocate input device\n");
		goto err_input_dev_alloc_failed;
	}
	ts->input_dev->name = "synaptics-rmi-touchscreen";
	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(BTN_TOUCH, ts->input_dev->keybit);
	set_bit(BTN_2, ts->input_dev->keybit);
	set_bit(EV_ABS, ts->input_dev->evbit);

	set_bit(KEY_BACK, ts->input_dev->keybit);
	set_bit(KEY_HOME, ts->input_dev->keybit);
	set_bit(KEY_MENU, ts->input_dev->keybit);
	set_bit(KEY_SEARCH, ts->input_dev->keybit);

	printk(KERN_INFO "synaptics_ts_probe: max_x %d, max_y %d\n", max_x, max_y);
	printk(KERN_INFO "input_set_abs_params: mix_x %d, max_x %d, min_y %d, max_y %d\n",
		pdata->abs_x_min, pdata->abs_x_max, pdata->abs_y_min, pdata->abs_y_max);

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, pdata->abs_x_min, pdata->abs_x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, pdata->abs_y_min, pdata->abs_y_max, 0, 0);

	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 30, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, pdata->finger_support - 1, 0, 0);

#ifndef CONFIG_TOUCHSCREEN_COMPATIBLE_REPORT
	input_set_abs_params(ts->input_dev, ABS_MT_AMPLITUDE,
		0, ((255 << 16) | 15), 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION,
		0, ((1 << 31) | (pdata->abs_x_max << 16) | pdata->abs_y_max), 0, 0);
#endif

	ret = input_register_device(ts->input_dev);
	if (ret) {
		printk(KERN_ERR "TOUCH_ERR: synaptics_ts_probe: "
				"Unable to register %s input device\n",
				ts->input_dev->name);
		goto err_input_register_device_failed;
	}

	gl_ts = ts;

	if (client->irq) {
		ret = request_irq(client->irq, synaptics_ts_irq_handler, IRQF_TRIGGER_LOW,
				client->name, ts);
		if (ret == 0) {
			/* enable abs int */
			data[0] = (get_address_base(ts, 0x11, FUNCTION) ?
				BIT(get_address_base(ts, 0x11, INTR_SOURCE)) : 0) |
				(get_address_base(ts, 0x19, FUNCTION) ?
				BIT(get_address_base(ts, 0x19, INTR_SOURCE)) : 0);
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x01, CONTROL_BASE) + 1, data[0]);
			if (ret)
				free_irq(client->irq, ts);
		}
		if (ret == 0)
			ts->use_irq = 1;
		else
			dev_err(&client->dev, "TOUCH_ERR: request_irq failed\n");
	}
	if (!ts->use_irq) {
		hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ts->timer.function = synaptics_ts_timer_func;
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 1;
	ts->early_suspend.suspend = synaptics_ts_early_suspend;
	ts->early_suspend.resume = synaptics_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	synaptics_touch_sysfs_init();
	if (pdata->cable_support)
		usb_register_notifier(&cable_status_handler);

	printk(KERN_INFO "synaptics_ts_probe: Start touchscreen %s in %s mode\n", ts->input_dev->name, ts->use_irq ? "interrupt" : "polling");

	return 0;

err_input_register_device_failed:
	input_free_device(ts->input_dev);

err_input_dev_alloc_failed:
	destroy_workqueue(ts->syn_wq);

err_create_wq_failed:
err_detect_failed:
	kfree(ts);

err_alloc_data_failed:
err_check_functionality_failed:
	return ret;
}

static int synaptics_ts_remove(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	unregister_early_suspend(&ts->early_suspend);
	if (ts->use_irq)
		free_irq(client->irq, ts);
	else
		hrtimer_cancel(&ts->timer);
	input_unregister_device(ts->input_dev);

	synaptics_touch_sysfs_remove();

	kfree(ts);
	return 0;
}

static int synaptics_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);

	if (ts->use_irq)
		disable_irq(client->irq);
	else
		hrtimer_cancel(&ts->timer);
	ret = cancel_work_sync(&ts->work);
	if (ret && ts->use_irq) /* if work was pending disable-count is now 2 */
		enable_irq(client->irq);

	ts->pre_finger_data[0][0] = 0;

	if (ts->power)
		ts->power(0);
	else {
		ret = i2c_syn_write_byte_data(client,
			get_address_base(ts, 0x01, CONTROL_BASE), 0x01); /* sleep */
		if (ret < 0)
			printk(KERN_ERR "TOUCH_ERR: synaptics_ts_suspend: i2c_smbus_write_byte_data failed\n");
	}
	return 0;
}

static int synaptics_ts_resume(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	printk(KERN_INFO "%s: enter\n", __func__);

	if (ts->power) {
		ts->power(1);
		if (ts->cable_support) {
			if (usb_get_connect_type())
				ts->cable_config = ((ts->cable_config & 0xDF) | (1 << 5));
			printk(KERN_INFO "%s: ts->cable_config: %x\n", __func__, ts->cable_config);
		}
	} else {
		i2c_syn_write_byte_data(client,
			get_address_base(ts, 0x01, CONTROL_BASE), 0x00); /* wake */
		msleep(100);
	}

	synaptics_init_panel(ts);
	ts->timestamp = jiffies;

#ifdef CONFIG_TOUCHSCREEN_COMPATIBLE_REPORT
	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
	input_sync(ts->input_dev);
#else
	input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
	input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
#endif

	if (ts->use_irq)
		enable_irq(client->irq);

	if (!ts->use_irq)
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_suspend(ts->client, PMSG_SUSPEND);
}

static void synaptics_ts_late_resume(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_resume(ts->client);
}
#endif

static const struct i2c_device_id synaptics_ts_id[] = {
	{ SYNAPTICS_3K_NAME, 0 },
	{ }
};

static struct i2c_driver synaptics_ts_driver = {
	.probe		= synaptics_ts_probe,
	.remove		= synaptics_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= synaptics_ts_suspend,
	.resume		= synaptics_ts_resume,
#endif
	.id_table	= synaptics_ts_id,
	.driver = {
		.name	= SYNAPTICS_3K_NAME,
	},
};

static int __devinit synaptics_ts_init(void)
{
	return i2c_add_driver(&synaptics_ts_driver);
}

static void __exit synaptics_ts_exit(void)
{
	i2c_del_driver(&synaptics_ts_driver);
}

module_init(synaptics_ts_init);
module_exit(synaptics_ts_exit);

MODULE_DESCRIPTION("Synaptics Touchscreen Driver");
MODULE_LICENSE("GPL");
